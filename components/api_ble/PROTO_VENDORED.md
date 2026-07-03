# Vendored native-API proto layer

Verbatim copies from `esphome/components/api/` of ESPHome **2026.6.4**
(`~/dev/esphome`, `esphome/const.py` `__version__`). No edits were required —
all `#include` directives in these files are either local-relative (and the
files sit side by side here) or point at `esphome/core/...`.

Files:

- `proto.h` / `proto.cpp` — varint/wire-format helpers, `ProtoWriteBuffer`,
  `ProtoVarInt`, `ProtoMessage` / `ProtoDecodableMessage` base classes.
- `api_pb2.h` / `api_pb2.cpp` — generated message classes for `api.proto`.
- `api_pb2_dump.cpp` — generated `dump_to()` implementations (only compiled
  when the logger level is VERY_VERBOSE, via `HAS_PROTO_MESSAGE_DUMP`).
- `api_pb2_defines.h` — generated defines header (included by `proto.h`).
- `api_pb2_includes.h` — component-type includes for pointer-optimized fields
  (included by `api_pb2.h`).
- `api_buffer.h` / `api_buffer.cpp` — `APIBuffer` (no-zero-fill byte buffer)
  used by `ProtoWriteBuffer` (included by `proto.h`).

These files define symbols in namespace `esphome::api`; `api_ble` declares
`CONFLICTS_WITH = ["api"]` so both can never be linked into one image.

Regen procedure: re-copy the same file list from the pinned ESPHome checkout
after bumping it, then update the version above.
