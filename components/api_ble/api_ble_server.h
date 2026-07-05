#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "api_ble_connection.h"
#include "api_pb2.h"

#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/controller.h"
#include "esphome/core/string_ref.h"

#include <esp_gatts_api.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace esphome::api_ble {

/// GATT peripheral speaking the ESPHome native API protocol.
///
/// Owns one primary service (advertised) with an RX characteristic
/// (write / write-no-response, central -> device) and a TX characteristic
/// (notify + CCCD, device -> device). The CCCD subscription is the
/// "session open" signal: an APIBLEConnection is created when it lands and
/// destroyed on BLE disconnect.
///
/// Also a Controller: registered with ControllerRegistry so entity state
/// changes flow into the active session as push-on-change updates (same
/// mechanism the in-tree api component uses).
class APIBLEServer : public Component, public Controller {
 public:
  APIBLEServer();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_reboot_timeout(uint32_t reboot_timeout) { this->reboot_timeout_ = reboot_timeout; }
  void set_connection_params(uint16_t min_interval_ms, uint16_t max_interval_ms, uint16_t latency,
                             uint16_t timeout_ms) {
    this->conn_min_interval_ms_ = min_interval_ms;
    this->conn_max_interval_ms_ = max_interval_ms;
    this->conn_latency_ = latency;
    this->conn_timeout_ms_ = timeout_ms;
  }
#ifdef USE_API_BLE_NOISE
  void set_noise_psk(const std::array<uint8_t, 32> &psk) { this->noise_psk_ = psk; }
  const std::array<uint8_t, 32> &get_noise_psk() const { return this->noise_psk_; }
#endif

  /// esp32_ble's optional `name:` override, mirrored from YAML by codegen.
  /// truncate_adv_name_() needs the base GAP name esp32_ble computed, but
  /// esp32_ble exposes no getter and its GAP dispatch does not forward
  /// ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT, so the name cannot be read back.
  /// nullptr when not configured (the app name is used instead).
  void set_ble_name_override(const char *name) { this->ble_name_override_ = name; }

  /// Raw GATTS events, registered with esp32_ble via codegen (the ESPHome BLE
  /// wrapper does not surface MTU events, so we track ESP_GATTS_MTU_EVT
  /// ourselves; also used for the peer address on connect and the CCCD
  /// session-open signal).
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

  uint16_t get_mtu() const { return this->mtu_; }

  /// Called by the connection when the Hello handshake completes, so the
  /// client_info string is available for the connected trigger.
  void notify_client_hello(const std::string &client_info);

  /// Forward a device-originated Home Assistant action to the active session
  /// (dropped unless the client is service-subscribed -- in-tree semantics).
  void send_homeassistant_action(const api::HomeassistantActionRequest &req);

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
  // Action response tracking (mirrors APIServer). A HomeAssistantServiceCallAction
  // with on_success/on_error registers a callback keyed by the request's call_id;
  // the matching HomeassistantActionResponse from the client resolves it.
  using ActionResponseCallback = std::function<void(const class ActionResponse &)>;
  void register_action_response_callback(uint32_t call_id, ActionResponseCallback callback);
  void handle_action_response(uint32_t call_id, bool success, StringRef error_message);
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  void handle_action_response(uint32_t call_id, bool success, StringRef error_message, const uint8_t *response_data,
                              size_t response_data_len);
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

  // --- Home Assistant state import registry (mirrors APIServer) ---
  struct HomeAssistantStateSubscription {
    const char *entity_id;  // pointer to flash (from codegen)
    const char *attribute;  // pointer to flash, or nullptr (no attribute)
    std::function<void(StringRef)> callback;
    bool once;
  };
  void subscribe_home_assistant_state(const char *entity_id, const char *attribute,
                                      std::function<void(StringRef)> &&f);
  const std::vector<HomeAssistantStateSubscription> &get_state_subs() const { return this->state_subs_; }

  // Controller state-change callbacks (push-on-change into the session).
#ifdef USE_BINARY_SENSOR
  void on_binary_sensor_update(binary_sensor::BinarySensor *obj) override;
#endif
#ifdef USE_SENSOR
  void on_sensor_update(sensor::Sensor *obj) override;
#endif
#ifdef USE_TEXT_SENSOR
  void on_text_sensor_update(text_sensor::TextSensor *obj) override;
#endif
#ifdef USE_SWITCH
  void on_switch_update(switch_::Switch *obj) override;
#endif
#ifdef USE_LIGHT
  void on_light_update(light::LightState *obj) override;
#endif
#ifdef USE_COVER
  void on_cover_update(cover::Cover *obj) override;
#endif
#ifdef USE_FAN
  void on_fan_update(fan::Fan *obj) override;
#endif
#ifdef USE_CLIMATE
  void on_climate_update(climate::Climate *obj) override;
#endif
#ifdef USE_NUMBER
  void on_number_update(number::Number *obj) override;
#endif
#ifdef USE_SELECT
  void on_select_update(select::Select *obj) override;
#endif
#ifdef USE_LOCK
  void on_lock_update(lock::Lock *obj) override;
#endif
#ifdef USE_MEDIA_PLAYER
  void on_media_player_update(media_player::MediaPlayer *obj) override;
#endif

#ifdef USE_API_BLE_CLIENT_CONNECTED_TRIGGER
  Trigger<std::string, std::string> *get_client_connected_trigger() { return &this->client_connected_trigger_; }
#endif
#ifdef USE_API_BLE_CLIENT_DISCONNECTED_TRIGGER
  Trigger<std::string, std::string> *get_client_disconnected_trigger() { return &this->client_disconnected_trigger_; }
#endif

 protected:
  static constexpr uint16_t INVALID_CONN_ID = 0xFFFF;
  /// Default ATT MTU before an MTU exchange happens (=> 20-byte chunks).
  static constexpr uint16_t DEFAULT_MTU = 23;

  void setup_service_();
  void truncate_adv_name_();
  void on_ble_connect_(uint16_t conn_id);
  void on_ble_disconnect_(uint16_t conn_id);
  void open_session_(uint16_t conn_id);
  void close_session_();
  void request_connection_params_(const uint8_t *remote_bda);
  std::string peer_address_str_() const;

  esp32_ble_server::BLEService *service_{nullptr};
  esp32_ble_server::BLECharacteristic *rx_char_{nullptr};
  esp32_ble_server::BLECharacteristic *tx_char_{nullptr};

  std::unique_ptr<APIBLEConnection> connection_;
  uint16_t conn_id_{INVALID_CONN_ID};
  uint16_t mtu_{DEFAULT_MTU};
  uint16_t cccd_handle_{0};
  /// TX characteristic attribute handle, captured at ESP_GATTS_ADD_CHAR_EVT
  /// (the wrapper does not expose it) for direct esp_ble_gatts_send_indicate.
  uint16_t tx_char_handle_{0};
  /// Bluedroid congestion state (ESP_GATTS_CONGEST_EVT); pauses the TX drain.
  bool congested_{false};
  uint8_t remote_bda_[6]{};

  std::vector<HomeAssistantStateSubscription> state_subs_;

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
  struct PendingActionResponse {
    uint32_t call_id;
    ActionResponseCallback callback;
  };
  std::vector<PendingActionResponse> action_response_callbacks_;
#endif

  /// esp32_ble `name:` override (pointer to a string literal in flash), see
  /// set_ble_name_override().
  const char *ble_name_override_{nullptr};

  uint32_t reboot_timeout_{0};
  uint32_t last_connected_ms_{0};
  bool client_ever_connected_{false};

  uint16_t conn_min_interval_ms_{30};
  uint16_t conn_max_interval_ms_{60};
  uint16_t conn_latency_{0};
  uint16_t conn_timeout_ms_{5000};

#ifdef USE_API_BLE_NOISE
  std::array<uint8_t, 32> noise_psk_{};
#endif

#ifdef USE_API_BLE_CLIENT_CONNECTED_TRIGGER
  Trigger<std::string, std::string> client_connected_trigger_;
#endif
#ifdef USE_API_BLE_CLIENT_DISCONNECTED_TRIGGER
  Trigger<std::string, std::string> client_disconnected_trigger_;
#endif
};

/// Fill `mac` with the device's Bluetooth MAC (base eFuse MAC + BT offset),
/// which is the address the device advertises over BLE. The HA side matches the
/// companion esphome config entry by unique_id == format_mac(<BLE address>), so
/// both the DeviceInfoResponse and the Noise server-hello must report this BT
/// MAC (not the base eFuse MAC) or discovery and Noise handshake cross-checks
/// won't line up.
void get_bt_mac_raw(uint8_t mac[6]);

/// Global pointer for platform components (api_ble sensor/text_sensor) to reach
/// the server for Home Assistant state subscriptions. Mirrors
/// api::global_api_server.
extern APIBLEServer *global_api_ble_server;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
