#include "serial_console.h"

#ifdef USE_ESP32

#include "esphome/components/logger/logger.h"
#include "esphome/core/log.h"

#include <driver/uart.h>

namespace esphome::serial_console {

static const char *const TAG = "serial_console";

void SerialConsole::setup() {
  // Piggyback on the UART the logger already configured. The logger's RX
  // buffer is tiny (hardware FIFO + 1, ~129 bytes), so hosts must send short
  // lines and the loop() poll must stay frequent -- fine for a dev console.
  auto *log = logger::global_logger;
  if (log == nullptr || log->get_baud_rate() == 0) {
    ESP_LOGW(TAG, "Logger UART disabled (baud_rate 0); serial console inactive");
    this->mark_failed();
    return;
  }
  const uart_port_t num = log->get_uart_num();
  if (!uart_is_driver_installed(num)) {
    ESP_LOGW(TAG, "No UART driver on port %d (USB-CDC/JTAG logger?); serial console inactive", num);
    this->mark_failed();
    return;
  }
  this->uart_num_ = num;
}

void SerialConsole::loop() {
  if (this->uart_num_ < 0)
    return;
  uint8_t buf[64];
  int len = uart_read_bytes(static_cast<uart_port_t>(this->uart_num_), buf, sizeof(buf), 0);
  if (len <= 0)
    return;
  for (int i = 0; i < len; i++) {
    const char c = static_cast<char>(buf[i]);
    if (c == '\n' || c == '\r') {
      if (!this->line_.empty()) {
        ESP_LOGD(TAG, "Command: '%s'", this->line_.c_str());
        this->command_callbacks_.call(this->line_);
        this->line_.clear();
      }
      continue;
    }
    if (this->line_.size() < MAX_LINE) {
      this->line_.push_back(c);
    }
  }
}

void SerialConsole::dump_config() {
  ESP_LOGCONFIG(TAG, "Serial console: %s (uart %d)", this->is_failed() ? "INACTIVE" : "active", this->uart_num_);
}

}  // namespace esphome::serial_console

#endif  // USE_ESP32
