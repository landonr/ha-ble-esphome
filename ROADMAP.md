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
7. **media_player**: compile-gate is in place but never built or exercised —
   add to a test config with a speaker platform.
8. ~~**`homeassistant.action` response capture** (`capture_response`,
   `on_success`/`on_error`)~~ — done 2026-07-04. Ports the in-tree api
   response machinery: `on_success`/`on_error` set a per-call `call_id` on the
   `HomeassistantActionRequest` and register a callback in `APIBLEServer`;
   the client's `HomeassistantActionResponse` (msg 130, gated by
   `USE_API_HOMEASSISTANT_ACTION_RESPONSES`) matches the `call_id` and fires the
   success/error trigger. `capture_response: true` additionally requests the
   JSON body (`USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON`, `json` auto-loaded)
   and fires the success trigger with the parsed object; `response_template`
   supported. No transport work needed — the response rides the transparent
   byte pipe. Compile-gated on both families (`feature-test.yaml` C6 + Noise +
   JSON, `m5stack-fire-compile-test.yaml` classic ESP32). Live validation on
   the Fire rig still pending.
9. ~~**esphome-entry linking fallback** in the HA integration~~ — done
   2026-07-04. Both linking gaps close on the same key: the device now reports
   its **BT MAC** (the BLE-advertised address) in DeviceInfo and the Noise
   server-hello, so `format_mac(our BLE address) == esphome entry unique_id`.
   `_async_ensure_esphome_entry`'s fallback (no stored `entry_id`) matches the
   esphome entry by `unique_id == our_mac` + host==`127.0.0.1` (the old dead
   `hable_address` tag match is gone), recovering after a lost `entry_id`. For
   the Noise-key case the programmatic flow returns a FORM, so a one-shot
   `_async_await_esphome_entry` dispatcher listener
   (`SIGNAL_CONFIG_ENTRY_CHANGED`, ADDED/UPDATED, verified on HA 2026.6.4) links
   and repoints the entry the moment the user finishes the key prompt in the UI,
   then unsubscribes; `entry.async_on_unload` guarantees it dies with the entry
   if the user never finishes. New `_async_repoint_esphome_entry` helper rewrites
   host/port only when stale. Migration caveat: the reported-MAC change (was the
   base eFuse MAC) invalidates pre-existing esphome entries — one-time fix is to
   delete the stale esphome entry, keep the hable entry, and let it recreate on
   restart.
10. ~~**Multi-device**: second api_ble peripheral, proxy slot budgeting, and
    `max_connections > 1` on the device side.~~ — done 2026-07-04. Device side
    is **slot coexistence only**: `max_connections > 1` needs no api_ble code
    change (it only sizes the radio's connection slots for co-resident BLE
    components like a proxy — advertising resume stays gated by `max_clients`,
    which is pinned to 1); `feature-test.yaml` gets `esp32_ble: max_connections:
    3` as the coexistence compile case. Multi-central remains explicitly out of
    scope and is **now enforced** — `FINAL_VALIDATE_SCHEMA` rejects
    `esp32_ble_server` `max_clients` ≠ 1, and runtime guards ignore MTU/congestion
    events from a non-active `conn_id`, refuse a second central's session-stealing
    CCCD subscribe, and actively drop a second central (`esp_ble_gatts_close`) to
    free its radio slot. Proxy slot budgeting is **documented math, not an
    allocator** (ARCHITECTURE.md §4 *Multi-device & proxy slot budget*): per-entry
    isolation on the HA side (one bridge/port/lock per hable entry), unique_id
    linking with the BT MAC as the cross-layer key, and a `3 − N` slot budget per
    proxy; reconnect jitter (`random.uniform(0, 1.0)` on the backoff) keeps N
    bridges from retrying in lockstep for scarce slots. Live two-device
    validation done 2026-07-05 on the Fire + Guition ESP32-4848S040 rig through
    the single XIAO C6 proxy (2/3 slots): both reflashed devices relinked by BT
    MAC (`3c:61:05:07:3a:46`, `28:84:85:86:1a:ee`), the one-time MAC migration
    (delete stale esphome entries, hable recreates) worked as documented, both
    entity sets live concurrently, commands round-tripped on both, and an HA
    restart relinked both esphome entries to their own fresh ephemeral ports
    (37747 / 41883) with no cross-linking. Not yet exercised live: the
    Noise-FORM listener (needs an encrypted reflash) and fault isolation
    (single-device reboot).
11. **Deep-sleep / battery device proof**: the original motivation — a
    battery sensor node (e.g. the XIAO C6 once freed from proxy duty) that
    wakes, connects, pushes states, sleeps.

## Phase 5 — Distribution

12. **Proto re-vendor script**: regenerate `api_pb2.*` from a given ESPHome
    checkout (documents the pinned version; see PROTO_VENDORED.md).
13. ~~**CI**: compile matrix (ESP32-C6 + classic ESP32) against the pinned
    ESPHome release; `py_compile`/ruff for the HA integration.~~ — done
    2026-07-05. `.github/workflows/ci.yml`: compile matrix over `xiao-c6`,
    `feature-test` (C6) and `m5stack-fire-compile-test` (classic ESP32)
    against pip-pinned ESPHome (version mirrors PROTO_VENDORED.md — bump
    together), PlatformIO cache per target; ruff + `py_compile` job for
    `custom_components/hable`.
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
