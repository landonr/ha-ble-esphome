# Hable macOS BLE ↔ TCP bridge (`mac_bridge.py`)

A standalone development tool for testing the Hable **ESPHome native API over
BLE** pipeline from a Mac, without touching the production Home Assistant. It
holds a GATT connection to a Hable device and exposes it as a localhost TCP
socket so that plain [`aioesphomeapi`](https://pypi.org/project/aioesphomeapi/)
(or Wireshark, or the stock `esphome` integration) can speak the native API
over the BLE byte pipe described in `../ARCHITECTURE.md` (sections 1, 2, 4).

The bridge is a **dumb byte pump** with zero protocol knowledge:

```
  TX notifications  ──►  TCP client
  TCP client bytes  ──►  RX characteristic writes (chunked to MTU−3)
```

It has no dependency on Home Assistant — only `bleak` and `aioesphomeapi`.

## Setup

```sh
cd tools
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## macOS Bluetooth permission (important)

On macOS, Bleak talks to **CoreBluetooth**, so:

- Device "addresses" are **CoreBluetooth peripheral UUIDs**, not MAC addresses.
  The `scan` output prints exactly the identifier that `bridge`/`test` accept.
  Matching by device **name** also works.
- The **terminal app** running this script needs Bluetooth permission:
  **System Settings → Privacy & Security → Bluetooth** → enable your terminal
  (Terminal, iTerm, VS Code, …). The **first run may prompt**, or **fail with a
  CoreBluetooth authorization error** (e.g. `BleakError: ... not authorized`)
  until permission is granted. Grant it and re-run.

## Usage

All commands take `-v/--verbose` for DEBUG logging (byte counters, per-attempt
connect detail).

### Scan for devices

```sh
python mac_bridge.py scan --timeout 8
```

Prints `ADDRESS  RSSI  NAME` for every device advertising the Hable service
UUID `ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01`.

### Run the bridge

```sh
# by CoreBluetooth UUID (from `scan`)
python mac_bridge.py bridge 1234ABCD-...-... --port 16053

# or by name
python mac_bridge.py bridge my-hable-device
```

Serves on `127.0.0.1:16053` (override with `--port`). One TCP client at a time;
a second connection is refused. On BLE disconnect it drops the TCP client and
reconnects in a loop. Point any ESPHome native-API client at
`127.0.0.1:<port>`.

### End-to-end test with aioesphomeapi

```sh
# connect, print device_info + entities, stream states for 30s
python mac_bridge.py test my-hable-device

# stream forever
python mac_bridge.py test my-hable-device --watch

# toggle a switch entity (by object_id) after listing entities
python mac_bridge.py test my-hable-device --toggle-switch my_switch

# encrypted device
python mac_bridge.py test my-hable-device --noise-psk <base64-psk>
```

`test` starts the bridge internally, then uses `aioesphomeapi` against
`127.0.0.1`: connects, prints `device_info`, lists entities, subscribes to
state updates, optionally toggles a switch, and prints updates for `--duration`
seconds (default 30) or until Ctrl-C with `--watch`.

## Notes

- Chunk size for RX writes is `max(20, mtu_size − 3)`. Writes use
  write-without-response when the RX characteristic advertises it, falling back
  to with-response automatically.
- Default port `16053` (the ESPHome native API port `6053` + 10000, to avoid
  clashing with a local ESPHome device).
