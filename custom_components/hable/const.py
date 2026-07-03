"""Constants for the Hable (ESPHome API over BLE) integration."""

from __future__ import annotations

DOMAIN = "hable"

# GATT service / characteristics (Nordic-UART-style byte pipe, custom UUIDs).
# See ARCHITECTURE.md section 2.
SERVICE_UUID = "ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01"
RX_CHAR_UUID = "ab1e0002-e5b0-4c8e-a9f3-6b5c2d3e4f01"  # central -> device (write)
TX_CHAR_UUID = "ab1e0003-e5b0-4c8e-a9f3-6b5c2d3e4f01"  # device -> central (notify)

# Config entry data keys.
CONF_ADDRESS = "address"
CONF_NAME = "name"

# Stored in our entry options once the companion esphome entry exists.
CONF_ESPHOME_ENTRY_ID = "esphome_entry_id"

# Bridge defaults.
BRIDGE_HOST = "127.0.0.1"
DEFAULT_MTU = 23  # BLE default ATT MTU before exchange
ATT_HEADER_SIZE = 3  # ATT write/notify opcode + handle overhead
MIN_CHUNK_SIZE = 20  # DEFAULT_MTU - ATT_HEADER_SIZE
# Every Nth GATT write uses response=True as coarse flow control.
FLOW_CONTROL_WRITE_INTERVAL = 16
