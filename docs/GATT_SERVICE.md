# Hable GATT Service Specification

Version 1.0 — matches `api_ble` device component and `hable` HA integration
as of 2026-07. This document is the wire contract: anything implementing
either side (device peripheral or bridging central) against it will
interoperate with the other.

## Overview

The service is a transparent, bidirectional **byte pipe** carrying the
[ESPHome native API protocol](https://esphome.io/components/api.html)
unmodified. There is **no BLE-level framing, fragmentation header, or
protocol logic** — native API frames are self-delimiting, so both sides
simply shuttle bytes. A conforming central can pump these bytes into any
existing native-API client (e.g. `aioesphomeapi`) byte-for-byte.

## Service and characteristics

One primary service, two characteristics (Nordic-UART-style, custom 128-bit
UUIDs):

| Item | UUID | Properties | Direction |
|---|---|---|---|
| Service | `ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01` | — | — |
| RX | `ab1e0002-e5b0-4c8e-a9f3-6b5c2d3e4f01` | write, write-without-response | central → device |
| TX | `ab1e0003-e5b0-4c8e-a9f3-6b5c2d3e4f01` | notify (CCCD `0x2902`) | device → central |

## Advertising

- The 128-bit service UUID is present in the primary advertisement. This is
  the discovery key — centrals match on it.
- The device name rides in the scan response.
- Advertising resumes automatically after a disconnect.

## Session lifecycle

1. Central connects and (optionally) negotiates ATT MTU.
2. Central subscribes to TX notifications (CCCD write). **The CCCD write is
   the "session open" signal** — the device buffers nothing for a client and
   sends nothing until the subscription lands. The session is keyed off the
   CCCD write, not the GATT connection.
3. Central writes the native API `HelloRequest` frame bytes to RX; the
   protocol proceeds exactly as over TCP (Hello, optional auth, DeviceInfo,
   ListEntities, SubscribeStates, …).
4. On native API `DisconnectRequest`, the device drops the entire BLE link
   and resumes advertising.
5. On BLE disconnect (either side, any reason), both sides discard all
   buffered bytes and restart the protocol from Hello on the next
   connection — identical semantics to a TCP connection drop.

**Single central.** The device accepts one API session. A second central's
connection is actively dropped and its CCCD subscribe is refused; do not
attempt concurrent sessions.

## Byte-stream semantics

- **Device → central**: pending TX bytes are chunked into notifications of
  at most `MTU − 3` bytes each. The central MUST concatenate notification
  payloads in arrival order into a continuous stream.
- **Central → device**: the central writes chunks (any size up to
  `MTU − 3`, write-without-response preferred) to RX; the device appends
  them to its RX stream and parses frames out of it.
- A frame may span many GATT packets; one GATT packet may carry several
  small frames. Chunk boundaries carry **no meaning** — reassembly is the
  frame parsing the native API protocol already defines.
- BLE guarantees ordered, reliable delivery per connection, matching TCP's
  contract as seen by the frame layer.
- Until an MTU exchange happens, assume the BLE default MTU of 23
  (20-byte chunks).

## Frame formats (carried verbatim, defined by the native API)

- **Plaintext**: `0x00` indicator byte, varint payload size, varint message
  type, payload.
- **Noise** (`Noise_NNpsk0_25519_ChaChaPoly_SHA256`, prologue
  `NoiseAPIInit` + client hello): `0x01` indicator byte, big-endian u16 body
  length, body. With a PSK configured the device is Noise-only: plaintext
  clients receive an explicit reject frame, then the link drops.

The device's reported MAC address (in both `DeviceInfoResponse` and the
Noise server hello) is its **BT MAC** — the same address it advertises
from. Centrals may rely on this equality as a linking key.

## Flow control

- Central → device: no explicit mechanism required. As a coarse safeguard
  the reference central issues every Nth write with response
  (write-with-response) so the peripheral's ACK paces sustained transfers.
- Device → central: the device paces itself against stack congestion; a
  central only needs to consume notifications promptly.
- The device closes the connection if its TX buffer overflows (a stalled or
  absent consumer) — same semantics as the native API's TCP send-buffer
  limit.

## Security

Trust lives entirely in the native API layer (Noise PSK), never in BLE:

- **No BLE pairing/bonding.** The link is stateless and works through
  BLE proxies.
- Noise is optional but recommended — plaintext over BLE is trivially
  sniffable, unlike a WPA2 LAN.
