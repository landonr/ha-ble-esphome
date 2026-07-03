# Hable — ESPHome Native API over BLE

ESPHome's native API transported over BLE GATT instead of WiFi/TCP. Same
protobuf protocol, same Home Assistant integration — the device just doesn't
need WiFi.

**Status: working end-to-end.** See [ROADMAP.md](ROADMAP.md) for what's next
and [ARCHITECTURE.md](ARCHITECTURE.md) for the design.

| Piece | Path | What it is |
|---|---|---|
| Device component | `components/api_ble/` | ESPHome external component: GATT peripheral speaking the native API (entities, states, commands, `homeassistant.action`, HA-state import). No `wifi`/`network` needed. |
| HA integration | `custom_components/hable/` | BLE central + byte bridge: discovers devices by service UUID, holds the GATT link (local adapter or ESPHome bluetooth proxies), exposes each device as a localhost TCP socket the **stock** esphome integration connects to. |
| Dev tool | `tools/mac_bridge.py` | macOS BLE↔TCP bridge + `aioesphomeapi` test client for testing without HA. |
| Test configs | `tests/device/` | `m5stack-fire-demo.yaml` (menu demo), `c6-bt-proxy.yaml` (proxy), `feature-test.yaml` / `xiao-c6.yaml` (compile coverage). |

## Quick start (device)

```yaml
esp32_ble:
esp32_ble_server:
api_ble:            # instead of `api:` — conflicts with it by design

external_components:
  - source:
      type: local   # or git, once published
      path: components
```

Build with ESPHome ≥ 2026.6 (vendored proto version — see
`components/api_ble/PROTO_VENDORED.md`).

## Quick start (Home Assistant)

Copy `custom_components/hable/` into `/config/custom_components/`, restart HA.
Devices advertising the Hable service UUID are discovered automatically;
each gets a companion "stock esphome" entry pointing at the local bridge.
If your host adapter can't connect (weak USB radios are common), any ESPHome
bluetooth proxy near the device works — that's the recommended radio.
