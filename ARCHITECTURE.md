# Hable — ESPHome Native API over BLE

Transport swap for ESPHome's native API: BLE GATT instead of TCP. Same protobuf
messages, same frame formats on the wire, so Home Assistant's stock `esphome`
integration and `aioesphomeapi` work unchanged.

Two halves, one repo:

- `components/api_ble/` — ESPHome external component. GATT peripheral that
  speaks the native API protocol (self-contained; does not require or load the
  in-tree `api` component).
- `custom_components/hable/` — Home Assistant custom integration. BLE central
  that discovers devices, holds the GATT connection, and exposes each device as
  a localhost TCP socket that the stock `esphome` integration connects to.

## 1. Core model: BLE as a byte pipe

Native API frames are self-delimiting:

- **Plaintext**: `0x00` indicator, varint payload size, varint message type, payload.
- **Noise**: `0x01` indicator, big-endian u16 body length, body (handshake or
  ciphertext; decrypted payload carries its own u16 type + u16 length header,
  16-byte ChaChaPoly MAC as footer). Protocol `Noise_NNpsk0_25519_ChaChaPoly_SHA256`,
  prologue `NoiseAPIInit` + client hello.

Because frames carry their own lengths, the BLE link needs **no additional
framing or fragmentation header**. Both sides treat GATT as a byte stream:

- Device → central: pending TX bytes are chunked into notifications of at most
  `MTU − 3` bytes each. The central concatenates notification payloads.
- Central → device: the central writes chunks to the RX characteristic; the
  device appends them to an RX buffer and parses frames out of the stream.

A frame may span many GATT packets; a GATT packet may contain several small
frames. Reassembly is exactly the frame parsing the protocol already defines.

Consequences:

- The HA-side bridge is a dumb byte pump — zero protocol knowledge.
- Noise encryption passes through end-to-end unchanged (§5).
- Wireshark/`aioesphomeapi` semantics are preserved byte-for-byte.

BLE guarantees ordered, reliable delivery per connection (notifications and
writes ride on L2CAP over a reliable link layer), matching TCP's contract as
seen by the frame layer. On disconnect both sides discard buffers and restart
the protocol from Hello — same as a TCP connection drop.

## 2. GATT service design

One primary service, two characteristics (Nordic-UART-style, custom UUIDs):

| Item | UUID | Properties | Direction |
|---|---|---|---|
| Service | `ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01` | — | — |
| RX | `ab1e0002-e5b0-4c8e-a9f3-6b5c2d3e4f01` | write, write-no-response | central → device |
| TX | `ab1e0003-e5b0-4c8e-a9f3-6b5c2d3e4f01` | notify (+ CCCD 0x2902) | device → central |

- **Advertising**: the 128-bit service UUID goes in the primary advertisement
  (16 of 31 bytes); the device name rides in the scan response (esp32_ble
  enables scan response by default). This is the discovery key for the HA
  integration's bluetooth matcher.
- **Connect flow**: central connects → negotiates MTU → subscribes to TX (CCCD
  write) → sends `HelloRequest` bytes to RX. The device buffers nothing for a
  client until the CCCD subscription lands; CCCD write is the "session open"
  signal.
- **MTU**: the ESPHome BLE wrapper doesn't surface MTU events, so `api_ble`
  registers its own GATTS event callback with `esp32_ble` and tracks
  `ESP_GATTS_MTU_EVT` per connection. Until an exchange happens, assume the
  default MTU 23 (20-byte chunks). BlueZ centrals typically negotiate ~517.
- **Long writes**: `esp32_ble_server` already handles prepared writes (up to
  512 B per attribute value); the RX handler just consumes whatever length
  arrives.
- **Connections**: one central per device to start (`max_connections: 1`).
  The slot math (`esp32_ble` `max_connections`, default 3) stays user-tunable.

### TX flow control

The ESPHome wrapper has no notify congestion feedback — `notify()` returns
void and only logs when Bluedroid's queue is full. `api_ble` therefore owns a
TX ring buffer and bypasses the wrapper on the send path:

- `send bytes` = append to ring (fail/close connection if ring full — same
  semantics as the TCP helper's overflow buffer limit).
- Each `loop()` drains the ring in MTU-sized chunks via
  `esp_ble_gatts_send_indicate` directly (the TX characteristic's attribute
  handle is captured from our own `ESP_GATTS_ADD_CHAR_EVT` handler — the
  wrapper keeps it protected). The drain pauses while the stack reports
  congestion (`ESP_GATTS_CONGEST_EVT`) and stops-and-retries on send errors;
  a generous per-loop chunk cap (16) only bounds main-loop time.
- Ring sized to cover the initial burst (DeviceInfo + ListEntities + initial
  states — a few KB); configurable.

## 3. Device component (`components/api_ble/`)

Self-contained by decision: the in-tree `api` component's classes are `final`
and socket-bound (`socket::Socket` is a non-virtual typedef), and `api` hard
depends on `network`. `api_ble` does not touch it.

- **Vendored proto layer**: `proto.h/.cpp`, `api_pb2.h/.cpp` copied verbatim
  from the ESPHome checkout (they are generated from `api.proto` and contain no
  socket code). Because these define symbols in `esphome::api`,
  `api_ble` declares **`CONFLICTS_WITH = ["api"]`** — a device runs the API
  over TCP or over BLE, not both. (Revisit if coexistence is ever wanted.)
- **`APIBLEServer`** (Component): owns the GATT service (created via
  `esp32_ble_server`'s `BLEServer::create_service`, following the
  `esp32_improv` pattern — service created from `loop()` once the server is
  running), advertising registration, and the connection table.
- **`BLEBytePipe`**: per-connection byte stream. RX buffer fed by the RX
  characteristic's `on_write` (main-loop context, already serialized), TX ring
  drained into notifications as above, negotiated MTU tracking.
- **`APIBLEConnection`**: lean reimplementation of the connection state machine
  over a `BLEBytePipe`: frame parsing (plaintext, or Noise-only when a PSK is
  configured — `USE_API_BLE_NOISE`), Hello/DeviceInfo/Ping/Disconnect handling,
  ListEntities + SubscribeStates with initial-state dump then push-on-change
  (reusing `ComponentIterator` from core), SubscribeHomeassistantServices, and
  command dispatch for the supported entity subset.
- **Entity scope (MVP)**: states out — `binary_sensor`, `sensor`,
  `text_sensor`, plus `switch` and `light` (needed to receive commands and
  reflect them); commands in — `switch`, `light` (on/off/RGB/brightness),
  `media_player` (volume/mute) when present.
- **`homeassistant.action`**: same YAML grammar as `api:` (alias
  `homeassistant.service`), producing `HomeassistantActionRequest` sent to the
  connected, service-subscribed client; silently dropped otherwise (identical
  to `api` semantics). Implemented as our own `HomeAssistantServiceCallAction`
  registered under the same action names (possible because `api` is absent).

### YAML

```yaml
esp32_ble:
  # peripheral role; api_ble consumes one connection slot

api_ble:
  encryption:            # optional, same as api:
    key: !secret ble_api_key
  reboot_timeout: 15min  # same semantics as api:
  connection:            # battery knobs
    min_interval: 30ms   # requested via esp_ble_gap_update_conn_params
    max_interval: 60ms
    latency: 4
    timeout: 5s
  on_client_connected: ...
  on_client_disconnected: ...
```

`DEPENDENCIES = ["esp32", "esp32_ble_server"]`. **No `network`, no `wifi`** —
the component must config-validate and run with WiFi absent. (BLE + WiFi coex
is also supported; ESP-IDF handles radio sharing.)

Codegen registers the component's GATTS/GAP handler counts with `esp32_ble` so
the static callback managers are sized correctly (same mechanism
`esp32_ble_server` uses).

### Keepalive / lifecycle

Same as `api`: respond to client pings; send our own `PingRequest` after the
keepalive window and drop the session if unanswered. `reboot_timeout` reboots
when no client has connected in the window. On BLE disconnect the session is
destroyed; advertising resumes automatically (`esp32_ble_server` restarts it).

## 4. HA integration (`custom_components/hable/`)

Custom integration + in-process localhost TCP bridge. The stock `esphome`
integration remains the entity layer; hable is transport only.

- **Discovery**: `manifest.json` bluetooth matcher on the service UUID →
  config flow's `async_step_bluetooth`. Runs in-process with `habluetooth`, so
  it sees the local BlueZ adapter today (the live deployment: one Realtek USB
  radio, zero ESPHome bluetooth proxies) and would see proxy-backed scanners
  later without code changes.
- **Per device** (one hable config entry each):
  1. Resolve `BLEDevice` via `bluetooth.async_ble_device_from_address(...,
     connectable=True)`; connect with
     `bleak_retry_connector.establish_connection`.
  2. Subscribe to TX notifications; open `asyncio.start_server` on
     `127.0.0.1:0` (ephemeral port).
  3. Pump bytes: TCP→RX-characteristic writes (chunked to `client.mtu_size −
     3`, write-without-response with periodic with-response writes for flow
     control); TX-notifications→TCP. Backpressure BLE→TCP: past a write-buffer
     high-water mark (`TCP_WRITE_HIGH_WATER`, 256 KiB) the stalled TCP client
     is dropped — the esphome integration reconnects and the protocol
     restarts from Hello.
  4. Create — or update the host/port of — a stock `esphome` config entry
     pointing at `127.0.0.1:<port>`. Noise PSK entry/storage is handled by the
     esphome integration's own flow, untouched.
- **Single TCP client** per bridge socket (the esphome integration holds one
  connection); additional connects are refused.
- **Reconnection**: the bridge holds the GATT connection persistently. If BLE
  drops: close the TCP client (the esphome integration's `ReconnectLogic`
  starts backing off), register a `bluetooth.async_register_callback` for the
  address, and run a fallback retry timer on a 1/2/5/10/30 s backoff
  (`RECONNECT_BACKOFF`; the device resumes advertising immediately after a
  drop and the cached `BLEDevice` is usually still resolvable, so the first
  retry fires 1 s in rather than waiting for a relayed advertisement).
  Whichever trigger fires first reconnects; then accept the next TCP connect.
  RSSI dropouts thus degrade to the same UX as a WiFi node going offline.
- **`allow_service_calls`**: enforced by the esphome integration per entry as
  today (device-initiated actions are dropped unless the user enables the
  option) — hable inherits the gate for free.

Why not BLE-native in the integration: the esphome integration is
host/port-native end-to-end and `aioesphomeapi` hard-codes `socket.socket`;
forking the entity layer is large and version-fragile. The byte bridge is ~zero
protocol surface and survives HA upgrades.

## 5. Security model

- **Noise optional, mirroring `api:`.** The pipe carries
  `Noise_NNpsk0_25519_ChaChaPoly_SHA256` end-to-end: the device authenticates
  the client via the 32-byte PSK, traffic is AEAD-encrypted, and the BLE layer
  needs no trust. Same `encryption: key:` YAML; the esphome integration prompts
  for the key exactly as for WiFi nodes. Device side implemented in
  `noise_session.{h,cpp}` — a lean transcription of the in-tree
  `APINoiseFrameHelper` over the byte pipe (byte-identical wire format,
  including server hello and explicit-reject frames). With a PSK configured
  the connection is Noise-only: plaintext clients get a reject frame, then
  the link drops (in-tree semantics).
- **Plaintext allowed** for bring-up and parity with `api:` (note: BLE is
  trivially sniffable vs. a WPA2 LAN — docs will recommend enabling Noise).
- **No BLE pairing/bonding**: keeps the link stateless, works through any
  future proxy transport, avoids the ESP32 wrapper's thin SMP surface.

## 6. Battery / radio behavior

- Peripheral requests connection parameters (`esp_ble_gap_update_conn_params`)
  from YAML (`connection:` block): longer intervals + slave latency for
  battery devices at the cost of state-update latency.
- `batch_delay`-style coalescing falls out of the TX ring: states arriving in
  the same loop share notifications.
- Advertising interval left at esp32_ble defaults for MVP (configurable later).

## 7. Test / validation plan

- Device target: **Seeed XIAO ESP32-C6 on USB** (`seeed_xiao_esp32c6`,
  ESP-IDF). `tests/device/xiao-c6.yaml` with template binary_sensor / sensor /
  text_sensor / switch / light and an `on_...: homeassistant.action` trigger.
- `esphome config` + `esphome compile` via `~/dev/esphome/venv`.
- HA side validated against the live instance (HA 2026.6.4) over the MCP
  server: watch the hable + esphome entries appear, entities update, and a
  test script (to be created — `script.set_light_rgb` does not exist on the
  instance) fire via `homeassistant.action`.

## 8. Open risks

1. **Throughput/latency of initial sync.** DeviceInfo + ListEntities + initial
   states over 20-byte chunks (pre-MTU-exchange or MTU-23 centrals) would be
   slow; mitigation: hold TX until MTU exchange completes (BlueZ negotiates
   immediately), and keep MVP entity lists small.
2. **Bluedroid notify congestion.** ~~No CONF feedback; the retry-on-error
   drain is coarse.~~ Resolved: the drain calls `esp_ble_gatts_send_indicate`
   directly and pauses on `ESP_GATTS_CONGEST_EVT` / send errors. If it still
   proves lossy under load, switch TX to indications with an in-flight window
   of 1 (slower but ACKed) behind a config flag.
3. **ESPHome wrapper is BLE 4.2-featured** (`CONFIG_BT_BLE_42_FEATURES_SUPPORTED`;
   no DLE/2M-PHY knobs exposed). Effective throughput ~constrained; likely fine
   for state traffic, matters for logs/OTA (both out of scope for BLE MVP).
4. **Proto drift.** Vendored `api_pb2.*` pins us to an ESPHome release range;
   the HA esphome integration refuses APIs older than its minimum supported
   version. Mitigation: regen script documented; CI compile against the pinned
   checkout.
5. **Ephemeral bridge ports.** The esphome config entry's stored port must be
   rewritten by hable on every HA start (cross-domain `async_update_entry`) —
   functional but unconventional; watch for esphome integration races at
   startup. Alternative: deterministic per-device ports from a config range.
6. **WiFi-disabled compile path.** ESPHome without `wifi`/`network` on ESP32 is
   uncommon; other components in a user's config may still drag `network` in.
   The C6 test YAML compiles without WiFi to prove the path.
7. **One-central model.** HA is the only intended client; the esphome
   integration assumes exclusive API access anyway. Multi-central is explicitly
   out of scope.
8. **`CONFLICTS_WITH = ["api"]`** means no hybrid WiFi+BLE API on one device.
   Acceptable for the target use case (BLE replaces WiFi).

### Field findings (2026-07-03, live deployment)

- The HA host's Realtek USB adapter (hci1) **sees advertisements but cannot
  complete connections** to the device (10/10 attempts failed, zero connect
  events device-side). An ESPHome bluetooth proxy (XIAO C6,
  `tests/device/c6-bt-proxy.yaml`) next to the device works on the first try —
  treat proxies as the primary deployment radio, the local adapter as
  best-effort.
- Initial state dump must send **all** non-internal entities regardless of
  `has_state()` (in-tree behavior); skipping leaves entities `unavailable` in
  HA until their first change. (Fixed.)
- The device drops the whole BLE link on API `DisconnectRequest`; the bridge
  reconnects via the next relayed advertisement, observed latency up to ~2 min
  through a proxy. Addressed: the fallback timer now retries on a
  1/2/5/10/30 s backoff starting 1 s after the drop instead of a flat 30 s.
- aioesphomeapi still emits legacy message type 3 (ConnectRequest) on login;
  it no longer exists in the 2026.x proto and is safely ignored (warning
  downgraded to debug-worthy noise).
- End-to-end stack proven: Fire ⇄ BLE ⇄ C6 proxy ⇄ habluetooth ⇄ hable bridge
  ⇄ TCP ⇄ stock esphome integration; MTU 517, HA states (weather/media_player
  incl. attributes) delivered to device, entities live in HA.
