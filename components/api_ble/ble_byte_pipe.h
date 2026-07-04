#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include <esp_gatts_api.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
/// Flow control: drain() bypasses the esp32_ble_server wrapper (whose
/// notify() returns void, hiding congestion errors) and calls
/// esp_ble_gatts_send_indicate() directly. It stops when the Bluedroid stack
/// reports congestion (ESP_GATTS_CONGEST_EVT, tracked by APIBLEServer) or a
/// send fails, and retries on the next loop() iteration.
class BLEBytePipe {
 public:
  /// TX ring size: covers the initial burst (DeviceInfo + ListEntities +
  /// initial states). Configurable constant per architecture doc.
  static constexpr size_t TX_RING_SIZE = 4096;
  /// Upper bound on notifications per loop() -- bounds main-loop time only;
  /// real backpressure comes from the congestion feedback.
  static constexpr size_t MAX_NOTIFY_CHUNKS_PER_LOOP = 16;

  // --- RX side (fed by RX characteristic on_write) ---
  void feed_rx(std::span<const uint8_t> data);
  const uint8_t *rx_data() const { return this->rx_buffer_.data(); }
  /// Mutable view for in-place Noise decryption.
  uint8_t *rx_data_mutable() { return this->rx_buffer_.data(); }
  size_t rx_size() const { return this->rx_buffer_.size(); }
  /// Discard n consumed bytes from the front of the RX buffer.
  void rx_consume(size_t n);
  /// Discard all buffered RX bytes, keeping pending TX (e.g. so a queued
  /// Noise reject frame still drains out before teardown).
  void rx_clear();

  // --- TX side ---
  /// Append bytes to the TX ring. Returns false (and appends nothing) when
  /// the ring lacks space -- caller must treat this as a fatal connection
  /// error (same semantics as the TCP helper's overflow buffer limit).
  bool write(const uint8_t *data, size_t len);
  size_t tx_pending() const { return this->tx_size_; }
  size_t tx_free() const { return TX_RING_SIZE - this->tx_size_; }
  bool tx_empty() const { return this->tx_size_ == 0; }

  /// Drain pending TX bytes into notifications of min(mtu - 3, remaining)
  /// bytes each via esp_ble_gatts_send_indicate. Called from the owning
  /// component's loop(). `congested` is the stack's ESP_GATTS_CONGEST_EVT
  /// state; when set, nothing is sent this iteration.
  void drain(esp_gatt_if_t gatts_if, uint16_t conn_id, uint16_t attr_handle, uint16_t mtu, bool congested);

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
