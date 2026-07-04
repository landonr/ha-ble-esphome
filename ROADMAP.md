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
5. **Verify connection-parameter requests** (`connection:` YAML block) are
   actually accepted by centrals/proxies; measure battery impact on a
   battery-powered target.

## Phase 4 — Completeness

6. **More entity types**: cover, climate, fan, number, select, button, lock —
   currently binary_sensor / sensor / text_sensor / switch / light
   (+ media_player untested). Each is a small Info/State/Command triple in
   `api_ble_connection.cpp` following the existing pattern.
7. **media_player**: compile-gate is in place but never built or exercised —
   add to a test config with a speaker platform.
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

## Housekeeping

- `mac_bridge.py` reconnect polish (backoff/jitter) — dev tool only.
- Fire demo: reboot-timeout is 15 min with no client; fine for mains power,
  revisit for battery.
- The Samba mount at the scratchpad path is session-local; remount for the
  next deploy (creds in the Samba add-on options).
