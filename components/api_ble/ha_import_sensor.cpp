#include "ha_import_sensor.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32
#ifdef USE_SENSOR

#include "api_ble_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"

namespace esphome::api_ble {

static const char *const TAG = "api_ble.sensor";

void HomeassistantBLESensor::setup() {
  global_api_ble_server->subscribe_home_assistant_state(this->entity_id_, this->attribute_, [this](StringRef state) {
    auto val = parse_number<float>(state.c_str());
    if (!val.has_value()) {
      ESP_LOGW(TAG, "'%s': Can't convert '%s' to number!", this->entity_id_, state.c_str());
      this->publish_state(NAN);
      return;
    }
    if (this->attribute_ != nullptr) {
      ESP_LOGV(TAG, "'%s::%s': Got attribute state %.2f", this->entity_id_, this->attribute_, *val);
    } else {
      ESP_LOGV(TAG, "'%s': Got state %.2f", this->entity_id_, *val);
    }
    this->publish_state(*val);
  });
}

void HomeassistantBLESensor::dump_config() {
  LOG_SENSOR("", "Home Assistant BLE Sensor", this);
  ESP_LOGCONFIG(TAG, "  Entity ID: '%s'", this->entity_id_);
  if (this->attribute_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Attribute: '%s'", this->attribute_);
  }
}

}  // namespace esphome::api_ble

#endif  // USE_SENSOR
#endif  // USE_ESP32
#endif  // USE_API_BLE
