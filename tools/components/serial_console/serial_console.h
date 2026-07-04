#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <string>
#include <vector>

namespace esphome::serial_console {

/// Reads command lines from the logger's UART RX (which the logger installs
/// but never reads) and fires on_command triggers. Dev tool: lets the host
/// drive the device over the same USB cable that carries the logs.
class SerialConsole : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the logger has installed the UART driver.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void add_on_command_callback(std::function<void(std::string)> &&callback) {
    this->command_callbacks_.add(std::move(callback));
  }

 protected:
  static constexpr size_t MAX_LINE = 128;

  CallbackManager<void(std::string)> command_callbacks_;
  std::string line_;
  int uart_num_{-1};  // resolved in setup(); -1 = no readable UART driver
};

class CommandTrigger : public Trigger<std::string> {
 public:
  explicit CommandTrigger(SerialConsole *parent) {
    parent->add_on_command_callback([this](std::string value) { this->trigger(std::move(value)); });
  }
};

}  // namespace esphome::serial_console

#endif  // USE_ESP32
