#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32
#ifdef USE_SENSOR

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::api_ble {

/// Imports a Home Assistant entity's numeric state into a local sensor over the
/// api_ble transport. Mirrors homeassistant::HomeassistantSensor, but subscribes
/// through APIBLEServer instead of the in-tree api component.
class HomeassistantBLESensor : public sensor::Sensor, public Component {
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

#endif  // USE_SENSOR
#endif  // USE_ESP32
#endif  // USE_API_BLE
