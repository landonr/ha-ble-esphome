#!/usr/bin/env python3
"""Standalone macOS BLE <-> TCP bridge for the Hable ESPHome-over-BLE pipeline.

This is a development tool, independent of Home Assistant. It holds a GATT
connection to a Hable device and exposes it as a localhost TCP socket so that
plain ``aioesphomeapi`` (or the stock esphome integration, or Wireshark) can
speak the ESPHome native API over the BLE byte pipe described in
ARCHITECTURE.md sections 1, 2 and 4.

The bridge is a dumb byte pump with zero protocol knowledge:

    TX notification bytes  --->  TCP writer
    TCP reader bytes       --->  RX characteristic writes (chunked)

Subcommands:

    scan                 Discover advertising Hable devices.
    bridge <target>      Run the BLE<->TCP bridge (blocks).
    test   <target>      Run the bridge internally and drive it with
                         aioesphomeapi (device info, entities, state stream).

On macOS, Bleak uses CoreBluetooth, so device "addresses" are CoreBluetooth
peripheral UUIDs, not MAC addresses. ``scan`` prints exactly the identifier
that ``bridge``/``test`` accept, and matching by device name also works.

The terminal application running this script needs Bluetooth permission
(System Settings -> Privacy & Security -> Bluetooth). The first run may prompt
or fail with a CoreBluetooth authorization error until that is granted.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import sys
from typing import Any

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice

# GATT service / characteristics (see ARCHITECTURE.md section 2). Kept as
# local constants so this tool has no dependency on the HA integration package.
SERVICE_UUID = "ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01"
RX_CHAR_UUID = "ab1e0002-e5b0-4c8e-a9f3-6b5c2d3e4f01"  # central -> device (write)
TX_CHAR_UUID = "ab1e0003-e5b0-4c8e-a9f3-6b5c2d3e4f01"  # device -> central (notify)

BRIDGE_HOST = "127.0.0.1"
DEFAULT_PORT = 16053
DEFAULT_MTU = 23  # BLE default ATT MTU before exchange
ATT_HEADER_SIZE = 3  # ATT write/notify opcode + handle overhead
MIN_CHUNK_SIZE = 20  # DEFAULT_MTU - ATT_HEADER_SIZE

CONNECT_ATTEMPTS = 4
CONNECT_RETRY_DELAY = 2.0  # seconds between connect attempts
RECONNECT_DELAY = 3.0  # seconds before retrying after a live disconnect

_LOGGER = logging.getLogger("mac_bridge")


# ---------------------------------------------------------------------------
# Discovery helpers
# ---------------------------------------------------------------------------


def _adv_has_service(service_uuids: list[str] | None) -> bool:
    """Return True if the Hable service UUID is present in an advertisement."""
    if not service_uuids:
        return False
    return SERVICE_UUID.lower() in {u.lower() for u in service_uuids}


async def discover_hable_devices(
    timeout: float,
) -> list[tuple[BLEDevice, int]]:
    """Scan for advertisements carrying the Hable service UUID.

    Returns a list of ``(device, rssi)`` de-duplicated by address, keeping the
    strongest RSSI seen for each device.
    """
    found: dict[str, tuple[BLEDevice, int]] = {}
    discovered = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for device, adv in discovered.values():
        if not _adv_has_service(adv.service_uuids):
            continue
        rssi = adv.rssi if adv.rssi is not None else -127
        existing = found.get(device.address)
        if existing is None or rssi > existing[1]:
            found[device.address] = (device, rssi)
    return sorted(found.values(), key=lambda item: item[1], reverse=True)


async def resolve_device(target: str, timeout: float) -> BLEDevice | str:
    """Resolve a scan target (address or name) to a BLEDevice.

    Matches, in order: exact address (case-insensitive), exact name, then a
    name substring. Only devices advertising the Hable service are considered.
    If nothing matches but the target looks usable as an address, the raw
    target string is returned so BleakClient can attempt a direct connect.
    """
    target_l = target.strip().lower()
    devices = await discover_hable_devices(timeout)

    for device, _rssi in devices:
        if device.address.lower() == target_l:
            return device
    for device, _rssi in devices:
        if (device.name or "").lower() == target_l:
            return device
    for device, _rssi in devices:
        if target_l in (device.name or "").lower():
            return device

    if devices:
        names = ", ".join(
            f"{d.address} ({d.name or '?'})" for d, _ in devices
        )
        _LOGGER.warning(
            "No Hable device matched %r; advertising devices: %s",
            target,
            names,
        )
    else:
        _LOGGER.warning(
            "No Hable devices found while resolving %r; trying %r as a raw "
            "address",
            target,
            target,
        )
    # Fall back to a direct-address connect attempt.
    return target


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------


class MacBridge:
    """Bridge one BLE device to a localhost TCP socket (single client)."""

    def __init__(
        self,
        target: str,
        *,
        host: str = BRIDGE_HOST,
        port: int = DEFAULT_PORT,
        scan_timeout: float = 8.0,
    ) -> None:
        self._target = target
        self._host = host
        self._port = port
        self._scan_timeout = scan_timeout

        self._loop: asyncio.AbstractEventLoop | None = None
        self._client: BleakClient | None = None
        self._server: asyncio.Server | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._maintain_task: asyncio.Task[None] | None = None

        self._rx_supports_wnr = True
        self._stopping = False

        # Signalled by the disconnected callback (from an arbitrary thread).
        self._disconnected = asyncio.Event()
        # Set once a GATT connection with TX notifications is live.
        self._connected = asyncio.Event()

        self.bytes_ble_to_tcp = 0
        self.bytes_tcp_to_ble = 0

    @property
    def port(self) -> int:
        """Return the bound TCP port."""
        if self._server is not None and self._server.sockets:
            return self._server.sockets[0].getsockname()[1]
        return self._port

    @property
    def mtu_size(self) -> int:
        """Return the negotiated ATT MTU (default 23 until exchanged)."""
        if self._client is not None:
            return self._client.mtu_size or DEFAULT_MTU
        return DEFAULT_MTU

    def _chunk_size(self) -> int:
        return max(MIN_CHUNK_SIZE, self.mtu_size - ATT_HEADER_SIZE)

    # -- lifecycle ---------------------------------------------------------

    async def async_start(self, *, wait_for_ble: float | None = None) -> int:
        """Bind the TCP server and start the BLE connection maintainer.

        If ``wait_for_ble`` is given, block up to that many seconds for the
        first successful GATT connection before returning. Returns the bound
        TCP port.
        """
        self._loop = asyncio.get_running_loop()
        self._server = await asyncio.start_server(
            self._handle_tcp_client, host=self._host, port=self._port
        )
        _LOGGER.info("Bridge listening on %s:%s", self._host, self.port)

        self._maintain_task = asyncio.create_task(self._maintain_connection())

        if wait_for_ble is not None:
            try:
                await asyncio.wait_for(self._connected.wait(), timeout=wait_for_ble)
            except asyncio.TimeoutError as err:
                raise TimeoutError(
                    f"BLE device {self._target!r} did not connect within "
                    f"{wait_for_ble:.0f}s"
                ) from err
        return self.port

    async def async_stop(self) -> None:
        """Tear the bridge down."""
        self._stopping = True
        self._disconnected.set()

        if self._maintain_task is not None:
            self._maintain_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._maintain_task
            self._maintain_task = None

        if self._server is not None:
            self._server.close()
            with contextlib.suppress(Exception):
                await self._server.wait_closed()
            self._server = None

        self._close_tcp_client()
        await self._cleanup_client()

    async def serve_forever(self) -> None:
        """Run until cancelled (used by the ``bridge`` subcommand)."""
        assert self._maintain_task is not None
        await asyncio.shield(self._maintain_task)

    # -- BLE side ----------------------------------------------------------

    async def _maintain_connection(self) -> None:
        """Keep a GATT connection alive, reconnecting across disconnects."""
        while not self._stopping:
            try:
                await self._connect_ble()
            except Exception as err:  # noqa: BLE001 - report and keep retrying
                _LOGGER.error("BLE connect failed: %s", err)
                _LOGGER.debug("connect traceback", exc_info=True)
                await asyncio.sleep(RECONNECT_DELAY)
                continue

            self._disconnected.clear()
            self._connected.set()

            # Block until the device drops (or we are stopping).
            await self._disconnected.wait()
            self._connected.clear()
            if self._stopping:
                break

            _LOGGER.info("BLE disconnected; dropping TCP client and reconnecting")
            self._close_tcp_client()
            await self._cleanup_client()
            await asyncio.sleep(RECONNECT_DELAY)

    async def _connect_ble(self) -> None:
        """Resolve, connect (with retries), and subscribe to TX notifications."""
        last_err: Exception | None = None
        for attempt in range(1, CONNECT_ATTEMPTS + 1):
            device = await resolve_device(self._target, self._scan_timeout)
            label = device.address if isinstance(device, BLEDevice) else device
            _LOGGER.info(
                "Connecting to %s (attempt %d/%d)", label, attempt, CONNECT_ATTEMPTS
            )
            client = BleakClient(
                device, disconnected_callback=self._on_ble_disconnected
            )
            try:
                await client.connect()
                await client.start_notify(TX_CHAR_UUID, self._on_notification)
            except Exception as err:  # noqa: BLE001
                last_err = err
                _LOGGER.debug("connect attempt %d failed: %s", attempt, err)
                with contextlib.suppress(Exception):
                    await client.disconnect()
                if attempt < CONNECT_ATTEMPTS:
                    await asyncio.sleep(CONNECT_RETRY_DELAY)
                continue

            self._client = client
            self._detect_rx_write_mode(client)
            _LOGGER.info(
                "BLE connected to %s (mtu=%s, chunk=%d, rx_wnr=%s)",
                label,
                client.mtu_size,
                self._chunk_size(),
                self._rx_supports_wnr,
            )
            return

        raise RuntimeError(
            f"could not connect to {self._target!r} after {CONNECT_ATTEMPTS} "
            f"attempts: {last_err}"
        )

    def _detect_rx_write_mode(self, client: BleakClient) -> None:
        """Decide whether the RX characteristic supports write-without-response."""
        char: BleakGATTCharacteristic | None = None
        with contextlib.suppress(Exception):
            char = client.services.get_characteristic(RX_CHAR_UUID)
        if char is not None:
            self._rx_supports_wnr = "write-without-response" in char.properties
        else:
            # Characteristic metadata unavailable; assume WNR and fall back on
            # first write error if needed.
            self._rx_supports_wnr = True

    def _on_ble_disconnected(self, _client: BleakClient) -> None:
        """Bleak disconnected callback (may run off the event loop thread)."""
        if self._loop is None:
            return
        self._loop.call_soon_threadsafe(self._disconnected.set)

    def _on_notification(
        self, _characteristic: BleakGATTCharacteristic, data: bytearray
    ) -> None:
        """TX notification -> current TCP client."""
        writer = self._writer
        if writer is None or writer.is_closing():
            return  # no TCP client attached right now; drop
        self.bytes_ble_to_tcp += len(data)
        writer.write(bytes(data))
        _LOGGER.debug(
            "BLE->TCP +%d bytes (total %d)", len(data), self.bytes_ble_to_tcp
        )

    async def _cleanup_client(self) -> None:
        """Best-effort teardown of the current BleakClient."""
        client = self._client
        self._client = None
        if client is None:
            return
        with contextlib.suppress(Exception):
            if client.is_connected:
                await client.stop_notify(TX_CHAR_UUID)
        with contextlib.suppress(Exception):
            await client.disconnect()

    # -- TCP side ----------------------------------------------------------

    async def _handle_tcp_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Serve one TCP client at a time (the API consumer)."""
        peer = writer.get_extra_info("peername")
        if self._writer is not None:
            _LOGGER.warning("Refusing second TCP connection from %s", peer)
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
            return

        self._writer = writer
        _LOGGER.info("TCP client connected from %s", peer)
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                await self._write_ble(data)
        except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
            pass
        except Exception:  # noqa: BLE001
            _LOGGER.exception("TCP reader task failed")
        finally:
            if self._writer is writer:
                self._writer = None
            if not writer.is_closing():
                writer.close()
            _LOGGER.info("TCP client disconnected")

    async def _write_ble(self, data: bytes) -> None:
        """Chunk TCP bytes into GATT writes on the RX characteristic."""
        client = self._client
        if client is None or not client.is_connected:
            return  # BLE is down; drop (protocol restarts from Hello on reconnect)

        chunk_size = self._chunk_size()
        for offset in range(0, len(data), chunk_size):
            chunk = data[offset : offset + chunk_size]
            try:
                await client.write_gatt_char(
                    RX_CHAR_UUID, chunk, response=not self._rx_supports_wnr
                )
            except Exception as err:  # noqa: BLE001
                if self._rx_supports_wnr:
                    # Fall back to acknowledged writes for the rest of the stream.
                    _LOGGER.warning(
                        "write-without-response failed (%s); "
                        "falling back to with-response writes",
                        err,
                    )
                    self._rx_supports_wnr = False
                    await client.write_gatt_char(RX_CHAR_UUID, chunk, response=True)
                else:
                    raise
            self.bytes_tcp_to_ble += len(chunk)
        _LOGGER.debug(
            "TCP->BLE +%d bytes (total %d)", len(data), self.bytes_tcp_to_ble
        )

    def _close_tcp_client(self) -> None:
        writer = self._writer
        self._writer = None
        if writer is not None and not writer.is_closing():
            writer.close()


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------


async def cmd_scan(args: argparse.Namespace) -> int:
    """Scan for advertising Hable devices and print them."""
    _LOGGER.info(
        "Scanning %.0fs for service %s ...", args.timeout, SERVICE_UUID
    )
    devices = await discover_hable_devices(args.timeout)
    if not devices:
        print("No Hable devices found.")
        print(
            "(If a device is flashed and advertising, ensure this terminal has "
            "Bluetooth permission.)"
        )
        return 0

    print(f"Found {len(devices)} Hable device(s):")
    print(f"  {'ADDRESS (use with bridge/test)':40}  {'RSSI':>5}  NAME")
    for device, rssi in devices:
        print(f"  {device.address:40}  {rssi:>5}  {device.name or '?'}")
    return 0


async def cmd_bridge(args: argparse.Namespace) -> int:
    """Run the BLE<->TCP bridge until interrupted."""
    bridge = MacBridge(
        args.target,
        host=BRIDGE_HOST,
        port=args.port,
        scan_timeout=args.scan_timeout,
    )
    await bridge.async_start()
    print(
        f"Bridge running on {BRIDGE_HOST}:{bridge.port} -> {args.target}. "
        "Press Ctrl-C to stop."
    )
    try:
        await bridge.serve_forever()
    except asyncio.CancelledError:
        pass
    finally:
        await bridge.async_stop()
    return 0


async def cmd_test(args: argparse.Namespace) -> int:
    """Run the bridge internally and exercise it with aioesphomeapi."""
    # Imported lazily so `scan`/`bridge` do not require aioesphomeapi, but
    # still imported at module load below for `test --help` validation.
    from aioesphomeapi import APIClient
    from aioesphomeapi.core import APIConnectionError

    bridge = MacBridge(
        args.target,
        host=BRIDGE_HOST,
        port=args.port,
        scan_timeout=args.scan_timeout,
    )
    _LOGGER.info("Starting internal bridge; waiting for BLE connection ...")
    await bridge.async_start(wait_for_ble=args.connect_timeout)

    client = APIClient(
        BRIDGE_HOST,
        bridge.port,
        None,
        noise_psk=args.noise_psk,
        client_info="hable-mac-bridge-test",
    )

    switch_states: dict[int, bool] = {}
    switch_keys_by_object_id: dict[str, int] = {}

    def on_state(state: Any) -> None:
        key = getattr(state, "key", None)
        if key is not None and hasattr(state, "state") and key in switch_keys_by_object_id.values():
            with contextlib.suppress(Exception):
                switch_states[key] = bool(state.state)
        print(f"  state update: {state}")

    rc = 0
    try:
        await client.connect(login=False)
        info = await client.device_info()
        print("\n=== device_info ===")
        print(f"  name:            {info.name}")
        print(f"  model:           {info.model}")
        print(f"  esphome_version: {info.esphome_version}")
        print(f"  mac_address:     {info.mac_address}")
        print(f"  uses_password:   {info.uses_password}")

        entities, services = await client.list_entities_services()
        print(f"\n=== entities ({len(entities)}) ===")
        for ent in entities:
            object_id = getattr(ent, "object_id", "?")
            key = getattr(ent, "key", "?")
            print(f"  [{type(ent).__name__}] object_id={object_id} key={key} name={getattr(ent, 'name', '')!r}")
            if type(ent).__name__ == "SwitchInfo":
                switch_keys_by_object_id[object_id] = key
        if services:
            print(f"\n=== user services ({len(services)}) ===")
            for svc in services:
                print(f"  {getattr(svc, 'name', svc)}")

        print("\n=== subscribing to state updates ===")
        client.subscribe_states(on_state)

        if args.toggle_switch:
            await _toggle_switch(
                client, args.toggle_switch, switch_keys_by_object_id, switch_states
            )

        if args.watch:
            print("\nWatching state updates forever (Ctrl-C to stop) ...")
            while True:
                await asyncio.sleep(3600)
        else:
            print(f"\nWatching state updates for {args.duration:.0f}s ...")
            await asyncio.sleep(args.duration)

    except APIConnectionError as err:
        _LOGGER.error("aioesphomeapi connection error: %s", err)
        rc = 1
    except asyncio.CancelledError:
        pass
    finally:
        with contextlib.suppress(Exception):
            await client.disconnect()
        await bridge.async_stop()
    return rc


async def _toggle_switch(
    client: Any,
    object_id: str,
    keys_by_object_id: dict[str, int],
    states: dict[int, bool],
) -> None:
    """Flip a switch identified by object_id, if present."""
    key = keys_by_object_id.get(object_id)
    if key is None:
        _LOGGER.error(
            "No switch with object_id %r found (have: %s)",
            object_id,
            ", ".join(keys_by_object_id) or "none",
        )
        return
    # Give the initial state dump a moment to arrive.
    await asyncio.sleep(1.0)
    current = states.get(key, False)
    target = not current
    print(f"\n=== toggling switch {object_id!r}: {current} -> {target} ===")
    client.switch_command(key, target)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mac_bridge.py",
        description=(
            "macOS BLE<->TCP bridge for testing the Hable ESPHome-over-BLE "
            "pipeline with aioesphomeapi."
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="enable DEBUG logging (byte counters, per-attempt detail)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="discover advertising Hable devices")
    p_scan.add_argument(
        "--timeout", type=float, default=8.0, help="scan duration in seconds"
    )
    p_scan.set_defaults(func=cmd_scan)

    p_bridge = sub.add_parser(
        "bridge", help="run the BLE<->TCP bridge (blocks until Ctrl-C)"
    )
    p_bridge.add_argument("target", help="device address (CoreBluetooth UUID on macOS) or name")
    p_bridge.add_argument(
        "--port", type=int, default=DEFAULT_PORT, help="TCP listen port on 127.0.0.1"
    )
    p_bridge.add_argument(
        "--scan-timeout",
        type=float,
        default=8.0,
        help="per-attempt scan duration when resolving the device",
    )
    p_bridge.set_defaults(func=cmd_bridge)

    p_test = sub.add_parser(
        "test", help="run the bridge internally and drive it with aioesphomeapi"
    )
    p_test.add_argument("target", help="device address (CoreBluetooth UUID on macOS) or name")
    p_test.add_argument(
        "--port", type=int, default=DEFAULT_PORT, help="TCP port for the internal bridge"
    )
    p_test.add_argument(
        "--scan-timeout",
        type=float,
        default=8.0,
        help="per-attempt scan duration when resolving the device",
    )
    p_test.add_argument(
        "--connect-timeout",
        type=float,
        default=45.0,
        help="seconds to wait for the first BLE connection before giving up",
    )
    p_test.add_argument(
        "--duration",
        type=float,
        default=30.0,
        help="seconds to watch state updates (ignored with --watch)",
    )
    p_test.add_argument(
        "--watch", action="store_true", help="watch state updates forever"
    )
    p_test.add_argument(
        "--toggle-switch",
        metavar="OBJECT_ID",
        default=None,
        help="toggle the switch with this object_id once entities are listed",
    )
    p_test.add_argument(
        "--noise-psk",
        default=None,
        help="base64 Noise PSK if the device has encryption enabled",
    )
    p_test.set_defaults(func=cmd_test)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    try:
        return asyncio.run(args.func(args))
    except KeyboardInterrupt:
        print("\nInterrupted.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
