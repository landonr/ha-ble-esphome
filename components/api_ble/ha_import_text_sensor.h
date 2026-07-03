#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32
#ifdef USE_TEXT_SENSOR

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome::api_ble {

/// Imports a Home Assistant entity's string state into a local text_sensor over
/// the api_ble transport. Mirrors homeassistant::HomeassistantTextSensor.
class HomeassistantBLETextSensor : public text_sensor::TextSensor, public Component {
 public:
  void set_entity_id(const char *entity_id) { this->entity_id_ = entity_id; }
  void set_attribute(const char *attribute) { this->attribute_ = attribute; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  const char *entity_id_{nullptr};
  const char *attribute_{nullptr};
};

}  // namespace esphome::api_ble

#endif  // USE_TEXT_SENSOR
#endif  // USE_ESP32
#endif  // USE_API_BLE
