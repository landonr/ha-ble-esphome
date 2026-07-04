#include "ble_byte_pipe.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::api_ble {

static const char *const TAG = "api_ble.pipe";

void BLEBytePipe::feed_rx(std::span<const uint8_t> data) {
  if (data.empty())
    return;
  this->rx_buffer_.insert(this->rx_buffer_.end(), data.begin(), data.end());
}

void BLEBytePipe::rx_consume(size_t n) {
  if (n >= this->rx_buffer_.size()) {
    this->rx_buffer_.clear();
    return;
  }
  this->rx_buffer_.erase(this->rx_buffer_.begin(), this->rx_buffer_.begin() + static_cast<ptrdiff_t>(n));
}

void BLEBytePipe::rx_clear() { this->rx_buffer_.clear(); }

bool BLEBytePipe::write(const uint8_t *data, size_t len) {
  if (len > this->tx_free()) {
    ESP_LOGW(TAG, "TX ring full (%u pending, %u requested)", static_cast<unsigned>(this->tx_size_),
             static_cast<unsigned>(len));
    return false;
  }
  size_t write_pos = (this->tx_head_ + this->tx_size_) % TX_RING_SIZE;
  size_t first = std::min(len, TX_RING_SIZE - write_pos);
  std::memcpy(this->tx_ring_ + write_pos, data, first);
  if (first < len) {
    std::memcpy(this->tx_ring_, data + first, len - first);
  }
  this->tx_size_ += len;
  return true;
}

void BLEBytePipe::drain(esp_gatt_if_t gatts_if, uint16_t conn_id, uint16_t attr_handle, uint16_t mtu,
                        bool congested) {
  if (attr_handle == 0 || this->tx_size_ == 0 || congested)
    return;
  // Notification payload limit is ATT_MTU - 3 (1 byte opcode + 2 bytes handle).
  // Chunks are staged through a stack buffer: the ring may wrap, and
  // esp_ble_gatts_send_indicate copies the data into its own queue anyway.
  uint8_t chunk_buf[514];  // max ATT payload at MTU 517
  const size_t max_chunk = std::min<size_t>((mtu > 3) ? static_cast<size_t>(mtu - 3) : 20, sizeof(chunk_buf));

  for (size_t i = 0; i < MAX_NOTIFY_CHUNKS_PER_LOOP && this->tx_size_ > 0; i++) {
    size_t chunk = std::min(this->tx_size_, max_chunk);
    size_t first = std::min(chunk, TX_RING_SIZE - this->tx_head_);
    std::memcpy(chunk_buf, this->tx_ring_ + this->tx_head_, first);
    if (first < chunk) {
      std::memcpy(chunk_buf + first, this->tx_ring_, chunk - first);
    }
    esp_err_t err = esp_ble_gatts_send_indicate(gatts_if, conn_id, attr_handle, chunk, chunk_buf,
                                                /*need_confirm=*/false);
    if (err != ESP_OK) {
      // Bluedroid TX queue full (or link mid-teardown); keep the bytes and
      // retry next loop iteration.
      ESP_LOGVV(TAG, "send_indicate error %d; %u bytes deferred", err, static_cast<unsigned>(this->tx_size_));
      return;
    }
    this->tx_head_ = (this->tx_head_ + chunk) % TX_RING_SIZE;
    this->tx_size_ -= chunk;
  }
}

void BLEBytePipe::reset() {
  this->rx_buffer_.clear();
  this->rx_buffer_.shrink_to_fit();
  this->tx_head_ = 0;
  this->tx_size_ = 0;
}

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
