#include "ha_import_text_sensor.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32
#ifdef USE_TEXT_SENSOR

#include "api_ble_server.h"

#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"

namespace esphome::api_ble {

static const char *const TAG = "api_ble.text_sensor";

void HomeassistantBLETextSensor::setup() {
  global_api_ble_server->subscribe_home_assistant_state(this->entity_id_, this->attribute_, [this](StringRef state) {
    if (this->attribute_ != nullptr) {
      ESP_LOGD(TAG, "'%s::%s': Got attribute state '%s'", this->entity_id_, this->attribute_, state.c_str());
    } else {
      ESP_LOGD(TAG, "'%s': Got state '%s'", this->entity_id_, state.c_str());
    }
    this->publish_state(state.c_str(), state.size());
  });
}

void HomeassistantBLETextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "Home Assistant BLE Text Sensor", this);
  ESP_LOGCONFIG(TAG, "  Entity ID: '%s'", this->entity_id_);
  if (this->attribute_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Attribute: '%s'", this->attribute_);
  }
}

}  // namespace esphome::api_ble

#endif  // USE_TEXT_SENSOR
#endif  // USE_ESP32
#endif  // USE_API_BLE
