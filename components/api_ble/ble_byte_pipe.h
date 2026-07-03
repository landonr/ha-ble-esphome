#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esphome::esp32_ble_server {
class BLECharacteristic;
}  // namespace esphome::esp32_ble_server

namespace esphome::api_ble {

/// Per-connection byte stream over BLE GATT.
///
/// RX: bytes arrive from the RX characteristic's on_write hook (main-loop
/// context, already serialized by the ESPHome BLE wrapper -- no locks needed)
/// and are appended to a plain buffer that the frame parser consumes from.
///
/// TX: bytes to send are appended to a fixed-size ring buffer that loop()
/// drains into notifications of at most (MTU - 3) bytes each.
///
/// NOTE (flow control): BLECharacteristic::notify() returns void -- an
/// esp_ble_gatts_send_indicate() congestion error is only logged inside
/// esp32_ble_server, so this pipe cannot see it. As backpressure we drain at
/// most MAX_NOTIFY_CHUNKS_PER_LOOP chunks per loop iteration instead of the
/// architecture doc's stop-on-error scheme. Phase 2 option: capture the TX
/// characteristic handle from our own GATTS ADD_CHAR event and call
/// esp_ble_gatts_send_indicate() directly for real error feedback.
class BLEBytePipe {
 public:
  /// TX ring size: covers the initial burst (DeviceInfo + ListEntities +
  /// initial states). Configurable constant per architecture doc.
  static constexpr size_t TX_RING_SIZE = 4096;
  /// Bounded number of notifications sent per loop() as coarse backpressure.
  static constexpr size_t MAX_NOTIFY_CHUNKS_PER_LOOP = 4;

  // --- RX side (fed by RX characteristic on_write) ---
  void feed_rx(std::span<const uint8_t> data);
  const uint8_t *rx_data() const { return this->rx_buffer_.data(); }
  size_t rx_size() const { return this->rx_buffer_.size(); }
  /// Discard n consumed bytes from the front of the RX buffer.
  void rx_consume(size_t n);

  // --- TX side ---
  /// Append bytes to the TX ring. Returns false (and appends nothing) when
  /// the ring lacks space -- caller must treat this as a fatal connection
  /// error (same semantics as the TCP helper's overflow buffer limit).
  bool write(const uint8_t *data, size_t len);
  size_t tx_pending() const { return this->tx_size_; }
  size_t tx_free() const { return TX_RING_SIZE - this->tx_size_; }
  bool tx_empty() const { return this->tx_size_ == 0; }

  /// Drain pending TX bytes into notifications on tx_char, chunked to
  /// min(mtu - 3, remaining) bytes. Called from the owning component's loop().
  void drain(esp32_ble_server::BLECharacteristic *tx_char, uint16_t mtu);

  /// Discard all buffered bytes (connection teardown).
  void reset();

 protected:
  std::vector<uint8_t> rx_buffer_;
  uint8_t tx_ring_[TX_RING_SIZE];
  size_t tx_head_{0};  // read position
  size_t tx_size_{0};  // bytes pending
};

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
