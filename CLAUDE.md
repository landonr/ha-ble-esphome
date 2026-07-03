# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESPHome's native API protocol transported over BLE GATT instead of WiFi/TCP.
Read `ARCHITECTURE.md` first — it holds the design decisions and field
findings; `ROADMAP.md` holds prioritized next work. Both are kept current.

The load-bearing idea: **the BLE link is a transparent byte pipe.** Native API
frames are self-delimiting, so both sides just shuttle bytes — no extra
framing layer. Anything that breaks byte-stream transparency breaks Noise
support and HA compatibility.

## Commands

ESPHome CLI comes from a separate checkout's venv (never `pip install esphome`):

```bash
~/dev/esphome/venv/bin/esphome config  tests/device/<file>.yaml   # validate
~/dev/esphome/venv/bin/esphome compile tests/device/<file>.yaml   # build
~/dev/esphome/venv/bin/esphome run --device /dev/cu.SLAB_USBtoUART --no-logs tests/device/m5stack-fire-demo.yaml   # flash M5 Fire
~/dev/esphome/venv/bin/esphome logs --device /dev/cu.SLAB_USBtoUART tests/device/m5stack-fire-demo.yaml           # serial logs
```

- `~/dev/esphome` is also the reference source for the in-tree `api`,
  `esp32_ble`, `esp32_ble_server` components — read it, never modify it.
- `tests/device/secrets.yaml` is gitignored; re-copy from
  `~/dev/esphome-devices/secrets.yaml` if missing.
- Compile coverage: `xiao-c6.yaml` + `feature-test.yaml` (ESP32-C6) and
  `m5stack-fire-compile-test.yaml` (classic ESP32) should all stay green —
  they gate both chip families.

HA integration has no test suite yet; sanity check is:

```bash
python3 -m py_compile custom_components/hable/*.py
```

BLE testing without Home Assistant (macOS, venv in `tools/.venv`):

```bash
cd tools && .venv/bin/python mac_bridge.py scan
.venv/bin/python mac_bridge.py test <address> --toggle-switch <object_id>
```

Do not run `mac_bridge` while HA's bridge is connected — the device accepts a
single central.

## Deploying to the live HA instance

- Copy `custom_components/hable/` to the HA `config` share over Samba
  (credentials are in the Samba add-on options, readable via the ha-mcp
  server). Python changes need a full HA restart, not an entry reload.
- The HA instance is production. Read freely via ha-mcp; ask before service
  calls that change device state. HA REST API token lives in `.mcp.json`
  (gitignored) — useful for driving config flows programmatically
  (`POST /api/config/config_entries/flow`).
- Current rig: M5Stack Fire (demo device, CP2104 serial) + XIAO C6 flashed as
  a standard bluetooth proxy (`c6-bt-proxy.yaml`). The HA host's Realtek USB
  adapter sees advertisements but cannot complete connections — the proxy is
  the working radio; don't burn time debugging the Realtek path.

## Architecture (what you'd otherwise learn the hard way)

Three pieces bridge device to HA:
`components/api_ble` (GATT peripheral) → `custom_components/hable` (BLE
central + localhost-TCP byte pump) → **stock** `esphome` integration (all
entity logic; hable creates/repoints its config entry at `127.0.0.1:<port>`).

### Device component (`components/api_ble`)

- **Self-contained by explicit decision** — does not load or patch the
  in-tree `api` component (`CONFLICTS_WITH = ["api"]`). The in-tree classes
  are `final` and socket-bound, so reuse is impossible anyway.
- `api_pb2.*`, `proto.*`, `api_buffer.*` are **verbatim vendored** generated
  files pinned to an ESPHome release (`PROTO_VENDORED.md`). Never hand-edit
  them; keep them in `esphome::api` namespace.
- Layering: `APIBLEServer` (GATT service, session lifecycle, Controller
  push hooks, HA-state subscription registry) → `BLEBytePipe` (RX buffer +
  bounded-drain TX ring) → `APIBLEConnection` (frame parse, message dispatch,
  entity encoders). All BLE callbacks arrive serialized on the main loop —
  no locks anywhere, keep it that way.
- Session lifecycle is keyed off the TX characteristic's **CCCD write**
  (subscribe = session open), not the GATT connection. MTU tracked via our
  own `ESP_GATTS_MTU_EVT` handler because the esp32_ble_server wrapper
  doesn't surface it.
- Single central; on API `DisconnectRequest` the device drops the whole BLE
  link and resumes advertising.
- Initial state dump must send **all** non-internal entities regardless of
  `has_state()` — skipping leaves them `unavailable` in HA (learned in the
  field).
- New entity types follow the existing triple: Info encoder (ListEntities),
  state encoder + Controller `on_*_update` hook, command decoder. Gate with
  the type's `USE_*` define.

### HA integration (`custom_components/hable`)

- `bridge.py` is the heart and is deliberately protocol-ignorant. Its
  reconnect machinery is **single-flight** (one `establish_connection` at a
  time via `_connect_lock`, stale clients explicitly discarded, GATT write
  failure = BLE-down teardown, advertisement callback + 30 s fallback timer).
  Racing connects was a real production bug — stale BleakClients silently eat
  writes and hold proxy slots.
- Discovery matches the service UUID `ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01`
  (RX `…0002`, TX `…0003`; also in `const.py` and mirrored in the device
  component — change nowhere or everywhere).
- The companion esphome entry is created programmatically via the esphome
  user config flow (`{host, port}`); `allow_service_calls` must be enabled on
  it for `homeassistant.action` to execute.
