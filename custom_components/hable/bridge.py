"""BLE <-> localhost TCP byte pump for the Hable integration.

The bridge is a dumb byte pipe (see ARCHITECTURE.md section 1/4): it holds a
persistent GATT connection to the device and exposes it as a TCP socket on
127.0.0.1 that the stock esphome integration connects to. Zero protocol
knowledge lives here.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.exc import BleakError
from bleak_retry_connector import establish_connection

from homeassistant.components import bluetooth
from homeassistant.components.bluetooth.match import ADDRESS, BluetoothCallbackMatcher
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import async_call_later

from .const import (
    ATT_HEADER_SIZE,
    BRIDGE_HOST,
    DEFAULT_MTU,
    FLOW_CONTROL_WRITE_INTERVAL,
    MIN_CHUNK_SIZE,
    RX_CHAR_UUID,
    TX_CHAR_UUID,
)

_LOGGER = logging.getLogger(__name__)

# Fallback reconnect poll while disconnected, in case the advertisement
# callback never fires (observed with proxy-relayed advertisements).
RECONNECT_FALLBACK_INTERVAL = 30.0


class HableBridgeError(Exception):
    """Raised when the bridge cannot start."""


class HableBridge:
    """Bridge one BLE device to a localhost TCP socket."""

    def __init__(self, hass: HomeAssistant, address: str, name: str) -> None:
        """Initialize the bridge."""
        self._hass = hass
        self._address = address
        self._name = name

        self._client: BleakClient | None = None
        self._server: asyncio.Server | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._stopping = False

        # Serializes all connect attempts; only one establish_connection may
        # be in flight at a time (racing attempts leave stale clients that
        # hold proxy connection slots and break the write path).
        self._connect_lock = asyncio.Lock()

        # Set once the TCP server is bound.
        self.port: int | None = None

        # Coarse flow control: every Nth GATT write uses response=True.
        self._write_count = 0

        # Cancel handles for the reconnect triggers.
        self._cancel_adv_callback: Any | None = None
        self._cancel_fallback_timer: Any | None = None

        # Diagnostics counters.
        self.bytes_ble_to_tcp = 0
        self.bytes_tcp_to_ble = 0

    @property
    def ble_connected(self) -> bool:
        """Return True if the GATT connection is up."""
        return self._client is not None and self._client.is_connected

    @property
    def mtu_size(self) -> int:
        """Return the negotiated ATT MTU (default 23 until exchanged)."""
        if self._client is not None:
            return self._client.mtu_size or DEFAULT_MTU
        return DEFAULT_MTU

    async def async_start(self) -> int:
        """Connect BLE and start the TCP server; return the bound port."""
        await self._async_connect_ble()

        self._server = await asyncio.start_server(
            self._handle_tcp_client, host=BRIDGE_HOST, port=0
        )
        self.port = self._server.sockets[0].getsockname()[1]
        _LOGGER.debug(
            "%s: bridge listening on %s:%s", self._address, BRIDGE_HOST, self.port
        )
        return self.port

    async def async_stop(self) -> None:
        """Tear the bridge down."""
        self._stopping = True
        self._async_cancel_reconnect_triggers()

        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

        self._async_close_tcp_client()
        await self._async_discard_client()

    # ------------------------------------------------------------------
    # BLE side
    # ------------------------------------------------------------------

    async def _async_discard_client(self) -> None:
        """Drop the current BleakClient, best-effort disconnecting it.

        A stale client that is never disconnected can hold one of the scarce
        proxy connection slots until its supervision timeout.
        """
        client = self._client
        self._client = None
        if client is None:
            return
        try:
            if client.is_connected:
                await client.disconnect()
        except Exception:  # noqa: BLE001 - best-effort cleanup
            _LOGGER.debug("%s: stale client disconnect failed", self._address)

    async def _async_connect_ble(self) -> None:
        """Resolve the BLEDevice, connect, and subscribe to TX notifications.

        Serialized: concurrent callers coalesce — the second caller returns
        immediately if the first one connected.
        """
        async with self._connect_lock:
            if self._stopping or self.ble_connected:
                return
            await self._async_discard_client()

            ble_device = bluetooth.async_ble_device_from_address(
                self._hass, self._address, connectable=True
            )
            if ble_device is None:
                raise HableBridgeError(
                    f"Could not find BLE device with address {self._address}"
                )

            try:
                client = await establish_connection(
                    BleakClient,
                    ble_device,
                    self._name,
                    disconnected_callback=self._on_ble_disconnected,
                )
                # CCCD subscription is the "session open" signal for the
                # device (ARCHITECTURE.md section 2).
                await client.start_notify(TX_CHAR_UUID, self._on_notification)
            except (BleakError, TimeoutError, EOFError) as err:
                raise HableBridgeError(
                    f"BLE connection to {self._address} failed: {err}"
                ) from err

            if self._stopping:
                self._client = client
                await self._async_discard_client()
                return

            self._client = client
            self._async_cancel_reconnect_triggers()
            _LOGGER.info(
                "%s: BLE connected (mtu=%s)", self._address, client.mtu_size
            )

    def _on_notification(
        self, characteristic: BleakGATTCharacteristic, data: bytearray
    ) -> None:
        """Handle a TX notification: forward bytes to the current TCP client."""
        writer = self._writer
        if writer is None or writer.is_closing():
            # No esphome client connected right now; drop.
            return
        self.bytes_ble_to_tcp += len(data)
        writer.write(bytes(data))
        # TODO: backpressure. writer.drain() is async and this callback is
        # sync; if the TCP peer stalls, bytes buffer unboundedly in the
        # transport. Track transport.get_write_buffer_size() and pause
        # notifications / drop the client past a high-water mark.

    def _on_ble_disconnected(self, client: BleakClient) -> None:
        """Handle BLE disconnect (bleak disconnected_callback)."""
        if self._stopping or client is not self._client:
            # Stale client from a superseded connection; ignore.
            return
        # bleak invokes this from the event loop under HA's habluetooth
        # backend, but go through call_soon_threadsafe to be safe.
        self._hass.loop.call_soon_threadsafe(self._async_handle_ble_disconnect)

    @callback
    def _async_handle_ble_disconnect(self) -> None:
        """React to a BLE drop: kill the TCP client, arm reconnect triggers."""
        if self._stopping:
            return
        _LOGGER.info("%s: BLE disconnected", self._address)
        self._client = None
        # Closing the TCP socket kicks the esphome integration's
        # ReconnectLogic into its own backoff loop.
        self._async_close_tcp_client()
        self._async_arm_reconnect_triggers()

    @callback
    def _async_arm_reconnect_triggers(self) -> None:
        """Arm the advertisement callback and the fallback timer."""
        if self._stopping:
            return
        if self._cancel_adv_callback is None:
            self._cancel_adv_callback = bluetooth.async_register_callback(
                self._hass,
                self._async_device_advertised,
                BluetoothCallbackMatcher({ADDRESS: self._address}),
                bluetooth.BluetoothScanningMode.ACTIVE,
            )
        if self._cancel_fallback_timer is None:
            self._cancel_fallback_timer = async_call_later(
                self._hass, RECONNECT_FALLBACK_INTERVAL, self._async_fallback_tick
            )

    @callback
    def _async_cancel_reconnect_triggers(self) -> None:
        """Cancel the advertisement callback and fallback timer."""
        if self._cancel_adv_callback is not None:
            self._cancel_adv_callback()
            self._cancel_adv_callback = None
        if self._cancel_fallback_timer is not None:
            self._cancel_fallback_timer()
            self._cancel_fallback_timer = None

    @callback
    def _async_device_advertised(
        self,
        service_info: bluetooth.BluetoothServiceInfoBleak,
        change: bluetooth.BluetoothChange,
    ) -> None:
        """Device is advertising again; reconnect."""
        self._async_schedule_reconnect()

    @callback
    def _async_fallback_tick(self, _now: Any) -> None:
        """Periodic fallback: retry even without an advertisement."""
        self._cancel_fallback_timer = None
        if self._stopping or self.ble_connected:
            return
        self._async_schedule_reconnect()
        # Re-arm for the next interval; cancelled on successful connect.
        self._cancel_fallback_timer = async_call_later(
            self._hass, RECONNECT_FALLBACK_INTERVAL, self._async_fallback_tick
        )

    @callback
    def _async_schedule_reconnect(self) -> None:
        """Kick a reconnect attempt; concurrent kicks coalesce on the lock."""
        if self._stopping or self.ble_connected or self._connect_lock.locked():
            return
        self._hass.async_create_task(self._async_reconnect_ble())

    async def _async_reconnect_ble(self) -> None:
        """Re-establish the GATT connection after a drop."""
        if self._stopping or self.ble_connected:
            return
        try:
            await self._async_connect_ble()
        except HableBridgeError as err:
            _LOGGER.warning(
                "%s: BLE reconnect failed (%s); triggers stay armed",
                self._address,
                err,
            )
            self._async_arm_reconnect_triggers()

    # ------------------------------------------------------------------
    # TCP side
    # ------------------------------------------------------------------

    async def _handle_tcp_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Serve one TCP client (the esphome integration)."""
        if self._writer is not None:
            # Single-client policy: refuse a second connection immediately.
            _LOGGER.warning(
                "%s: refusing second TCP connection to bridge", self._address
            )
            writer.close()
            return

        self._writer = writer
        _LOGGER.debug("%s: TCP client connected", self._address)
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                if not await self._async_write_ble(data):
                    break
        except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
            pass
        except Exception:  # noqa: BLE001
            _LOGGER.exception("%s: TCP reader task failed", self._address)
        finally:
            if self._writer is writer:
                self._writer = None
            if not writer.is_closing():
                writer.close()
            _LOGGER.debug("%s: TCP client disconnected", self._address)

    async def _async_write_ble(self, data: bytes) -> bool:
        """Chunk TCP bytes into GATT writes; False if BLE is unusable."""
        client = self._client
        if client is None or not client.is_connected:
            # BLE is down; the disconnect handler closes the TCP side which
            # restarts the protocol from Hello on reconnect.
            return False

        chunk_size = max(
            MIN_CHUNK_SIZE, (client.mtu_size or DEFAULT_MTU) - ATT_HEADER_SIZE
        )
        try:
            for offset in range(0, len(data), chunk_size):
                chunk = data[offset : offset + chunk_size]
                self._write_count += 1
                # Coarse flow control: every Nth write waits for a response so
                # we do not overrun the peripheral's RX buffering.
                with_response = (
                    self._write_count % FLOW_CONTROL_WRITE_INTERVAL == 0
                )
                await client.write_gatt_char(
                    RX_CHAR_UUID, chunk, response=with_response
                )
                self.bytes_tcp_to_ble += len(chunk)
        except (BleakError, TimeoutError, EOFError) as err:
            # Writing into a dead link: treat as a BLE drop.
            _LOGGER.warning("%s: GATT write failed: %s", self._address, err)
            self._async_handle_ble_disconnect()
            return False
        return True

    @callback
    def _async_close_tcp_client(self) -> None:
        """Close the current TCP client, if any."""
        writer = self._writer
        self._writer = None
        if writer is not None and not writer.is_closing():
            writer.close()

    # ------------------------------------------------------------------
    # Diagnostics
    # ------------------------------------------------------------------

    def diagnostics(self) -> dict[str, Any]:
        """Return diagnostic state."""
        return {
            "ble_connected": self.ble_connected,
            "mtu_size": self.mtu_size,
            "tcp_port": self.port,
            "tcp_client_connected": self._writer is not None,
            "bytes_ble_to_tcp": self.bytes_ble_to_tcp,
            "bytes_tcp_to_ble": self.bytes_tcp_to_ble,
            "waiting_for_advertisement": self._cancel_adv_callback is not None,
        }
