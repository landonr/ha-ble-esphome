# Hable Roadmap

Status: **working end-to-end** (2026-07-03). M5Stack Fire demo live against
production HA through a XIAO C6 bluetooth proxy — states both ways, commands
both ways, `homeassistant.action` round trip proven. See ARCHITECTURE.md for
design and field findings.

## Phase 3 — Robustness (make it survive daily use)

Items 1–4 implemented 2026-07-03 (compile-gated; live validation of Noise on
the Fire rig still pending — flash with `encryption: key:` and let the stock
esphome integration prompt for the key).

1. ~~**Noise encryption over BLE**~~ — done. `NoiseSession`
   (`components/api_ble/noise_session.{h,cpp}`) transcribes the in-tree
   `APINoiseFrameHelper` handshake/crypto over the byte pipe; with a PSK
   configured the connection requires `0x01` frames and explicitly rejects
   plaintext. Compile-gated on both chip families (`feature-test.yaml`,
   `m5stack-fire-compile-test.yaml`).
2. ~~**Device TX flow control.**~~ — done. Drain now calls
   `esp_ble_gatts_send_indicate` directly (TX char handle captured at
   `ESP_GATTS_ADD_CHAR_EVT`), pauses on `ESP_GATTS_CONGEST_EVT`, and retries
   on send errors instead of the blind 4-chunks-per-loop cap.
3. ~~**Bridge TCP backpressure.**~~ — done. Write-buffer high-water mark
   (`TCP_WRITE_HIGH_WATER`, 256 KiB) → stalled esphome client is dropped and
   reconnects.
4. ~~**Reconnect latency.**~~ — done. Flat 30 s fallback timer replaced with
   a 1/2/5/10/30 s backoff (`RECONNECT_BACKOFF`) starting 1 s after the BLE
   drop; advertisement callback remains the fast path.
5. ~~**Verify connection-parameter requests**~~ — verified 2026-07-04 on the
   proxy path: esp32_ble swallows `ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT`, so the
   device now polls `esp_ble_get_current_conn_params()` 5 s after requesting
   and logs the verdict. Fire (90–120 ms, latency 2) via the XIAO C6 proxy:
   `Connection params accepted: interval 120 ms, latency 2, timeout 5000 ms`.
   Still open: macOS/CoreBluetooth central behavior (Apple clamps intervals),
   and measuring actual battery impact on a battery-powered target.

## Phase 4 — Completeness

6. ~~**More entity types**: cover, climate, fan, number, select, button,
   lock~~ — done 2026-07-04. Each is the standard Info/State/Command triple in
   `api_ble_connection.cpp` (button is command-only: ListEntities + press, no
   state, no Controller hook), with `on_*_update` Controller hooks in
   `api_ble_server.cpp`, mirroring the in-tree `api_connection.cpp` encoders.
   Compile-gated on both families: all seven added to `feature-test.yaml`
   (ESP32-C6 + Noise) and `m5stack-fire-compile-test.yaml` (classic ESP32).
   **Live-validated on the Fire rig 2026-07-04** through production HA over the
   BLE API: all seven appear (ListEntities), report initial state, and
   round-trip commands with state pushed back — cover (position), fan (speed),
   climate (two-point target temp), number (value), select (option), lock
   (lock/unlock), button (press). Demo entities live in
   `m5stack-fire-demo.yaml`. Base subset was
   binary_sensor / sensor / text_sensor / switch / light.
7. ~~**media_player**: compile-gate built + exercised~~ — build done
   2026-07-04. `m5stack-fire-mediaplayer-test.yaml` wires the modern
   `speaker` media_player (external I2S DAC + PSRAM audio pipeline) so the
   api_ble MediaPlayer Info/State/Command triple actually compiles and links
   against a real media_player component. Two hard constraints surfaced and
   punt real playback to item 16: (a) the stock `speaker` media_player
   hard-requires the `network` component (WiFi/Ethernet streaming) — declared
   bare here only to satisfy the dep; (b) internal-DAC output is no longer
   supported in ESPHome, and the Fire's speaker is internal-DAC. So on a
   BLE-only device the exercisable surface is volume / mute / pause / state,
   not URL playback.
8. **`homeassistant.action` response capture** (`capture_response`,
   `on_success`/`on_error`) — currently rejected at config time.
9. **esphome-entry linking fallback** in the HA integration: recover when our
   stored entry_id is lost; capture entry_id when the flow finishes in the UI
   (Noise-key case).
10. **Multi-device**: second api_ble peripheral, proxy slot budgeting, and
    `max_connections > 1` on the device side.
11. **Deep-sleep / battery device proof**: the original motivation — a
    battery sensor node (e.g. the XIAO C6 once freed from proxy duty) that
    wakes, connects, pushes states, sleeps.

## Phase 5 — Distribution

12. **Proto re-vendor script**: regenerate `api_pb2.*` from a given ESPHome
    checkout (documents the pinned version; see PROTO_VENDORED.md).
13. **CI**: compile matrix (ESP32-C6 + classic ESP32) against the pinned
    ESPHome release; `py_compile`/ruff for the HA integration.
14. **HACS packaging** for `custom_components/hable` (may need repo split or
    `hacs.json`), install docs, published GATT service spec.
15. **Upstreaming assessment**: ESPHome external-component registry for
    `api_ble`; longer-term, the in-tree seam (byte-stream abstraction under
    the api frame helpers) as an upstream PR if hybrid WiFi+BLE is ever
    wanted.
16. **Playing media over BLE** (deferred from item 7): actual audio playback
    on a BLE-only device. Blocked on two upstream realities found while
    building item 7 — the `speaker` media_player hard-requires the `network`
    component, and ESPHome dropped internal-DAC output (the Fire's speaker is
    internal-DAC). Needs either an external I2S DAC on the target and a way to
    stream media bytes over the BLE link (the media pipeline currently assumes
    network transport), or a rethink of the audio-source path. Only the
    control surface (volume/mute/pause/state) works over BLE today; the
    compile gate and BLE export are proven (item 7).

## Housekeeping

- `mac_bridge.py` reconnect polish (backoff/jitter) — dev tool only.
- Fire demo: reboot-timeout is 15 min with no client; fine for mains power,
  revisit for battery.
- The Samba mount at the scratchpad path is session-local; remount for the
  next deploy (creds in the Samba add-on options).
