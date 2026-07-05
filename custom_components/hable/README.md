# Hable — Home Assistant integration

Transport bridge that lets Home Assistant's **stock `esphome` integration**
talk to ESPHome devices over **Bluetooth LE** instead of WiFi/TCP. The device
side runs the companion ESPHome external component (`components/api_ble/`),
which speaks the unmodified ESPHome native API protocol over a
Nordic-UART-style GATT service.

Hable itself has zero protocol knowledge: per device it holds a persistent
GATT connection and pumps bytes between the BLE characteristics and a TCP
socket on `127.0.0.1:<ephemeral port>`, then creates (or re-points) a stock
`esphome` config entry at that socket. Entities, Noise encryption, and
`allow_service_calls` are all handled by the esphome integration exactly as
for a WiFi node.

See [ARCHITECTURE.md](../../ARCHITECTURE.md) for the full design.

## Install

### HACS (recommended)

1. HACS → ⋮ (top right) → **Custom repositories**.
2. Add `https://github.com/landonr/ha-ble-esphome`, type **Integration**.
3. Search for **Hable** in HACS and install it.
4. Restart Home Assistant.

### Manual

1. Copy `custom_components/hable/` into your Home Assistant config directory:
   `<config>/custom_components/hable/`.
2. Restart Home Assistant.

Requires a working Bluetooth adapter set up in Home Assistant (the
`bluetooth` integration); ESPHome Bluetooth proxies also work.

## How discovery works

The device advertises the Hable service UUID
(`ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01`). Home Assistant's bluetooth stack
matches it against this integration's manifest matcher and offers the device
under **Settings → Devices & Services → Discovered**. Confirming the flow:

1. Creates the hable entry (unique_id = BLE address) and starts the bridge:
   BLE connect → subscribe to TX notifications → listen on `127.0.0.1:0`.
2. Starts the esphome integration's own config flow pointed at the bridge
   socket. If the device uses Noise encryption, the esphome flow prompts for
   the key as usual.

Devices can also be added manually via **Add Integration → Hable**, which
lists currently discovered matching devices.

If the BLE link drops, the TCP socket is closed (the esphome integration
backs off and retries) and the bridge reconnects when the device advertises
again.
