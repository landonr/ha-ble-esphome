#include "ble_byte_pipe.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "esphome/components/esp32_ble_server/ble_characteristic.h"
#include "esphome/core/log.h"

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

void BLEBytePipe::drain(esp32_ble_server::BLECharacteristic *tx_char, uint16_t mtu) {
  if (tx_char == nullptr || this->tx_size_ == 0)
    return;
  // Notification payload limit is ATT_MTU - 3 (1 byte opcode + 2 bytes handle).
  const size_t max_chunk = (mtu > 3) ? static_cast<size_t>(mtu - 3) : 20;

  // notify() gives no congestion feedback (see header note), so cap the
  // number of chunks per loop iteration as coarse backpressure.
  for (size_t i = 0; i < MAX_NOTIFY_CHUNKS_PER_LOOP && this->tx_size_ > 0; i++) {
    size_t chunk = std::min(this->tx_size_, max_chunk);
    std::vector<uint8_t> payload(chunk);
    size_t first = std::min(chunk, TX_RING_SIZE - this->tx_head_);
    std::memcpy(payload.data(), this->tx_ring_ + this->tx_head_, first);
    if (first < chunk) {
      std::memcpy(payload.data() + first, this->tx_ring_, chunk - first);
    }
    tx_char->set_value(std::move(payload));
    tx_char->notify();
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
