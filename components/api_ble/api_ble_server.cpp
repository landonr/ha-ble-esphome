#include "api_ble_server.h"
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
#include "homeassistant_action.h"
#endif

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "esphome/components/esp32_ble_server/ble_2902.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_CONTROLLER_REGISTRY
#include "esphome/core/controller_registry.h"
#endif

#include <esp_gap_ble_api.h>
#include <esp_gatt_defs.h>
#include <esp_mac.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::api_ble {

using namespace esp32_ble_server;
using esp32_ble::ESPBTUUID;

static const char *const TAG = "api_ble";

static const char *const SERVICE_UUID = "ab1e0001-e5b0-4c8e-a9f3-6b5c2d3e4f01";
static const char *const RX_CHARACTERISTIC_UUID = "ab1e0002-e5b0-4c8e-a9f3-6b5c2d3e4f01";
static const char *const TX_CHARACTERISTIC_UUID = "ab1e0003-e5b0-4c8e-a9f3-6b5c2d3e4f01";

APIBLEServer *global_api_ble_server = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void get_bt_mac_raw(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_BT); }

APIBLEServer::APIBLEServer() { global_api_ble_server = this; }

void APIBLEServer::setup() {
  global_ble_server->on_connect([this](uint16_t conn_id) { this->on_ble_connect_(conn_id); });
  global_ble_server->on_disconnect([this](uint16_t conn_id) { this->on_ble_disconnect_(conn_id); });

#ifdef USE_CONTROLLER_REGISTRY
  // Receive entity state updates for push-on-change (same as APIServer).
  ControllerRegistry::register_controller(this);
#endif

  // Reboot timeout tracking (mirrors api_server.cpp)
  this->last_connected_ms_ = App.get_loop_component_start_time();
  if (this->reboot_timeout_ != 0) {
    this->status_set_warning(LOG_STR("waiting for client connection"));
  }
}

void APIBLEServer::loop() {
  if (!global_ble_server->is_running())
    return;

  // Lazily create the GATT service once the BLE server is up (esp32_improv
  // pattern), then start it once creation completes.
  if (this->service_ == nullptr) {
    this->setup_service_();
  }
  if (this->service_->is_created() && !this->service_->is_running() && !this->service_->is_starting()) {
    ESP_LOGD(TAG, "Starting service");
    this->service_->start();
  }

  if (this->connection_ != nullptr) {
    this->connection_->loop();
    this->connection_->pipe().drain(global_ble_server->get_gatts_if(), this->conn_id_, this->tx_char_handle_,
                                    this->mtu_, this->congested_);
    if (this->connection_->should_close() &&
        (!this->connection_->close_after_flush() || this->connection_->pipe().tx_empty())) {
      const uint16_t conn_id = this->conn_id_;
      this->close_session_();
      // Drop the BLE link too; advertising resumes automatically and the
      // protocol restarts from Hello on the next connection.
      if (conn_id != INVALID_CONN_ID) {
        esp_ble_gatts_close(global_ble_server->get_gatts_if(), conn_id);
      }
    }
    return;
  }

  // No session: check reboot timeout (done in loop to avoid scheduler churn,
  // mirrors api_server.cpp).
  if (this->reboot_timeout_ != 0) {
    const uint32_t now = App.get_loop_component_start_time();
    if (now - this->last_connected_ms_ > this->reboot_timeout_) {
      ESP_LOGE(TAG, "No clients; rebooting");
      App.safe_reboot();
    }
  }
}

// Advertised-name byte budget. The 31-byte scan response already carries the
// 128-bit service UUID (2 + 16 bytes; esp32_ble copies the adv payload into the
// scan response) and TX power (3 bytes); a Local Name entry costs another
// 2 bytes of header, leaving 8 bytes of name payload. Bluedroid drops an
// oversized Local Name instead of truncating it, so a longer GAP name means no
// name on the air at all and HA discovery falls back to showing the bare MAC.
static constexpr size_t MAX_ADV_NAME_LEN = 31 - (2 + 16) - 3 - 2;

void APIBLEServer::truncate_adv_name_() {
  // Rebuild the GAP name esp32_ble computed during its setup (there is no way
  // to read it back, see set_ble_name_override): the `name:` override plus MAC
  // suffix when enabled, otherwise the app name (which already carries the
  // suffix). Only the first MAX_ADV_NAME_LEN chars are kept, which also makes
  // esp32_ble's own 20-char cap (first 13 chars + last 7) irrelevant here.
  const char *base =
      this->ble_name_override_ != nullptr ? this->ble_name_override_ : App.get_name().c_str();
  // 7 = '-' plus last 6 MAC chars, appended by esp32_ble to a name override.
  const bool add_suffix = this->ble_name_override_ != nullptr && App.is_name_add_mac_suffix_enabled();
  if (strlen(base) + (add_suffix ? 7 : 0) <= MAX_ADV_NAME_LEN)
    return;  // already fits in the scan response; leave esp32_ble's name alone
  // With only 8 chars of budget the shared "hable-" node-name prefix would
  // leave two distinguishing chars; drop it before truncating ("tdisplay"
  // instead of "hable-td").
  static constexpr char PREFIX[] = "hable-";
  if (strncmp(base, PREFIX, sizeof(PREFIX) - 1) == 0)
    base += sizeof(PREFIX) - 1;
  char name[MAX_ADV_NAME_LEN + 1];
  size_t pos = 0;
  auto append = [&](const char *s, size_t len) {
    for (; len != 0 && pos < MAX_ADV_NAME_LEN; len--)
      name[pos++] = *s++;
  };
  append(base, strlen(base));
  if (add_suffix) {
    char mac[MAC_ADDRESS_BUFFER_SIZE];
    get_mac_address_into_buffer(mac);
    append("-", 1);
    append(mac + 6, 6);
  }
  name[pos] = '\0';
  esp_err_t err = esp_ble_gap_set_device_name(name);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gap_set_device_name failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGD(TAG, "GAP name truncated to '%s' to fit the scan response", name);
}

void APIBLEServer::setup_service_() {
  // Must happen before the service starts: starting an advertised service
  // triggers advertising (re)configuration, which snapshots the GAP name.
  this->truncate_adv_name_();

  ESP_LOGD(TAG, "Creating native API service");
  this->service_ = global_ble_server->create_service(ESPBTUUID::from_raw(SERVICE_UUID), /*advertise=*/true);

  this->rx_char_ = this->service_->create_characteristic(
      ESPBTUUID::from_raw(RX_CHARACTERISTIC_UUID),
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  this->rx_char_->on_write([this](std::span<const uint8_t> data, uint16_t conn_id) {
    // Buffer nothing until the CCCD subscription opened a session.
    if (this->connection_ != nullptr && conn_id == this->conn_id_) {
      this->connection_->pipe().feed_rx(data);
    }
  });

  this->tx_char_ =
      this->service_->create_characteristic(ESPBTUUID::from_raw(TX_CHARACTERISTIC_UUID), BLECharacteristic::PROPERTY_NOTIFY);
  // CCCD: BLECharacteristic hooks this descriptor's writes internally to
  // maintain its notify list; we additionally watch the raw GATTS write event
  // (gatts_event_handler) for the same descriptor as our session-open signal.
  this->tx_char_->add_descriptor(new BLE2902());  // NOLINT(cppcoreguidelines-owning-memory)
}

void APIBLEServer::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_CONNECT_EVT: {
      if (this->conn_id_ != INVALID_CONN_ID && this->conn_id_ != param->connect.conn_id)
        break;  // single-central model; extra centrals are ignored
      std::memcpy(this->remote_bda_, param->connect.remote_bda, sizeof(this->remote_bda_));
      this->request_connection_params_(param->connect.remote_bda);
      break;
    }
    case ESP_GATTS_MTU_EVT: {
      // The ESPHome BLE wrapper does not surface MTU events, hence our own
      // handler. Single-central model: while a connection is active, ignore MTU
      // events from any other central; otherwise track the latest negotiated MTU.
      if (this->conn_id_ != INVALID_CONN_ID && param->mtu.conn_id != this->conn_id_)
        break;
      this->mtu_ = param->mtu.mtu;
      ESP_LOGD(TAG, "MTU negotiated: %u (conn %u)", param->mtu.mtu, param->mtu.conn_id);
      break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
      // Capture the TX characteristic's attribute handle: the wrapper's
      // BLECharacteristic keeps it protected, and the drain path needs it
      // for direct esp_ble_gatts_send_indicate calls.
      if (this->service_ != nullptr && param->add_char.service_handle == this->service_->get_handle() &&
          ESPBTUUID::from_uuid(param->add_char.char_uuid) == ESPBTUUID::from_raw(TX_CHARACTERISTIC_UUID)) {
        this->tx_char_handle_ = param->add_char.attr_handle;
      }
      break;
    }
    case ESP_GATTS_CONGEST_EVT: {
      // Real TX flow control: the drain pauses while the stack is congested.
      // Single-central model: ignore congestion events from any other central.
      if (this->conn_id_ != INVALID_CONN_ID && param->congest.conn_id != this->conn_id_)
        break;
      this->congested_ = param->congest.congested;
      ESP_LOGV(TAG, "Congestion %s (conn %u)", this->congested_ ? "start" : "end", param->congest.conn_id);
      break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
      // Capture the handle of our TX CCCD so we can spot subscription writes.
      if (this->service_ != nullptr && param->add_char_descr.service_handle == this->service_->get_handle() &&
          param->add_char_descr.descr_uuid.len == ESP_UUID_LEN_16 &&
          param->add_char_descr.descr_uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
        this->cccd_handle_ = param->add_char_descr.attr_handle;
      }
      break;
    }
    case ESP_GATTS_WRITE_EVT: {
      if (this->cccd_handle_ == 0 || param->write.handle != this->cccd_handle_ || param->write.is_prep ||
          param->write.len < 2)
        break;
      const uint16_t cccd = encode_uint16(param->write.value[1], param->write.value[0]);
      if ((cccd & 0x0003) != 0) {
        this->open_session_(param->write.conn_id);
      } else if (this->connection_ != nullptr && param->write.conn_id == this->conn_id_) {
        // Unsubscribe = session close (central signaled end of interest).
        ESP_LOGD(TAG, "Client unsubscribed from TX");
        this->close_session_();
      }
      break;
    }
    default:
      break;
  }
}

void APIBLEServer::on_ble_connect_(uint16_t conn_id) {
  if (this->conn_id_ != INVALID_CONN_ID) {
    ESP_LOGW(TAG, "Second central connected (conn %u); dropping (single-central model)", conn_id);
    // Actively drop it to free the radio slot deterministically rather than
    // leaving a half-connected central holding a proxy connection.
    esp_ble_gatts_close(global_ble_server->get_gatts_if(), conn_id);
    return;
  }
  ESP_LOGD(TAG, "Central connected (conn %u)", conn_id);
  this->conn_id_ = conn_id;
  this->mtu_ = DEFAULT_MTU;  // until an MTU exchange happens
}

void APIBLEServer::on_ble_disconnect_(uint16_t conn_id) {
  if (conn_id != this->conn_id_)
    return;
  ESP_LOGD(TAG, "Central disconnected (conn %u)", conn_id);
  this->cancel_timeout("conn_params_verify");
  this->close_session_();
  this->conn_id_ = INVALID_CONN_ID;
  this->mtu_ = DEFAULT_MTU;
  this->congested_ = false;
}

void APIBLEServer::open_session_(uint16_t conn_id) {
  if (this->connection_ != nullptr)
    return;  // already open (e.g. CCCD rewritten)
  if (this->conn_id_ != INVALID_CONN_ID && conn_id != this->conn_id_) {
    // A second central's CCCD write must not steal the session before the
    // first central subscribes (single-central model).
    ESP_LOGV(TAG, "Ignoring CCCD subscribe from second central (conn %u)", conn_id);
    return;
  }
  this->conn_id_ = conn_id;
  this->connection_ = std::make_unique<APIBLEConnection>(this);
  this->client_ever_connected_ = true;
  if (this->reboot_timeout_ != 0) {
    this->status_clear_warning();
    this->last_connected_ms_ = App.get_loop_component_start_time();
  }
  // The connected trigger fires later, once the Hello handshake supplies
  // client_info (notify_client_hello).
}

void APIBLEServer::close_session_() {
  if (this->connection_ == nullptr)
    return;
  this->connection_.reset();
  if (this->reboot_timeout_ != 0) {
    this->status_set_warning(LOG_STR("waiting for client connection"));
    this->last_connected_ms_ = App.get_loop_component_start_time();
  }
#ifdef USE_API_BLE_CLIENT_DISCONNECTED_TRIGGER
  this->client_disconnected_trigger_.trigger("", this->peer_address_str_());
#endif
}

void APIBLEServer::request_connection_params_(const uint8_t *remote_bda) {
  esp_ble_conn_update_params_t conn_params{};
  std::memcpy(conn_params.bda, remote_bda, sizeof(conn_params.bda));
  // min/max interval in 1.25 ms units; supervision timeout in 10 ms units.
  conn_params.min_int = std::max<uint16_t>(6, static_cast<uint16_t>(this->conn_min_interval_ms_ * 4 / 5));
  conn_params.max_int = std::max<uint16_t>(conn_params.min_int, static_cast<uint16_t>(this->conn_max_interval_ms_ * 4 / 5));
  conn_params.latency = this->conn_latency_;
  conn_params.timeout = std::max<uint16_t>(10, static_cast<uint16_t>(this->conn_timeout_ms_ / 10));
  esp_err_t err = esp_ble_gap_update_conn_params(&conn_params);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gap_update_conn_params failed: %d", err);
    return;
  }
  // esp32_ble swallows ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT before dispatch, so
  // the negotiation result is invisible via callbacks. Instead poll the live
  // parameters once the link has settled and report requested vs. actual.
  const uint16_t req_min = conn_params.min_int, req_max = conn_params.max_int;
  this->set_timeout("conn_params_verify", 5000, [this, req_min, req_max]() {
    if (this->conn_id_ == INVALID_CONN_ID)
      return;
    esp_gap_conn_params_t cur{};
    if (esp_ble_get_current_conn_params(this->remote_bda_, &cur) != ESP_OK) {
      ESP_LOGW(TAG, "Could not read current connection params");
      return;
    }
    // interval in 1.25 ms units, timeout in 10 ms units
    const uint32_t interval_ms = cur.interval * 5 / 4;
    const bool accepted = cur.interval >= req_min && cur.interval <= req_max && cur.latency == this->conn_latency_;
    ESP_LOGI(TAG, "Connection params %s: interval %" PRIu32 " ms, latency %u, timeout %u ms (requested %u-%u ms, latency %u)",
             accepted ? "accepted" : "NOT accepted (central overrode)", interval_ms, cur.latency, cur.timeout * 10,
             this->conn_min_interval_ms_, this->conn_max_interval_ms_, this->conn_latency_);
  });
}

std::string APIBLEServer::peer_address_str_() const {
  char buf[18];
  format_mac_addr_upper(this->remote_bda_, buf);
  return std::string(buf);
}

void APIBLEServer::notify_client_hello(const std::string &client_info) {
#ifdef USE_API_BLE_CLIENT_CONNECTED_TRIGGER
  this->client_connected_trigger_.trigger(client_info, this->peer_address_str_());
#else
  (void) client_info;
#endif
}

void APIBLEServer::send_homeassistant_action(const api::HomeassistantActionRequest &req) {
  if (this->connection_ != nullptr) {
    this->connection_->send_homeassistant_action(req);
  }
}

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
void APIBLEServer::register_action_response_callback(uint32_t call_id, ActionResponseCallback callback) {
  this->action_response_callbacks_.push_back({call_id, std::move(callback)});
}

void APIBLEServer::handle_action_response(uint32_t call_id, bool success, StringRef error_message) {
  for (auto it = this->action_response_callbacks_.begin(); it != this->action_response_callbacks_.end(); ++it) {
    if (it->call_id == call_id) {
      auto callback = std::move(it->callback);
      this->action_response_callbacks_.erase(it);
      ActionResponse response(success, error_message);
      callback(response);
      return;
    }
  }
}

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
void APIBLEServer::handle_action_response(uint32_t call_id, bool success, StringRef error_message,
                                          const uint8_t *response_data, size_t response_data_len) {
  for (auto it = this->action_response_callbacks_.begin(); it != this->action_response_callbacks_.end(); ++it) {
    if (it->call_id == call_id) {
      auto callback = std::move(it->callback);
      this->action_response_callbacks_.erase(it);
      ActionResponse response(success, error_message, response_data, response_data_len);
      callback(response);
      return;
    }
  }
}
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

void APIBLEServer::subscribe_home_assistant_state(const char *entity_id, const char *attribute,
                                                  std::function<void(StringRef)> &&f) {
  this->state_subs_.push_back(HomeAssistantStateSubscription{
      .entity_id = entity_id, .attribute = attribute, .callback = std::move(f), .once = false});
}

#ifdef USE_BINARY_SENSOR
void APIBLEServer::on_binary_sensor_update(binary_sensor::BinarySensor *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_binary_sensor_state(obj);
}
#endif
#ifdef USE_SENSOR
void APIBLEServer::on_sensor_update(sensor::Sensor *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_sensor_state(obj);
}
#endif
#ifdef USE_TEXT_SENSOR
void APIBLEServer::on_text_sensor_update(text_sensor::TextSensor *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_text_sensor_state(obj);
}
#endif
#ifdef USE_SWITCH
void APIBLEServer::on_switch_update(switch_::Switch *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_switch_state(obj);
}
#endif
#ifdef USE_LIGHT
void APIBLEServer::on_light_update(light::LightState *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_light_state(obj);
}
#endif
#ifdef USE_COVER
void APIBLEServer::on_cover_update(cover::Cover *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_cover_state(obj);
}
#endif
#ifdef USE_FAN
void APIBLEServer::on_fan_update(fan::Fan *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_fan_state(obj);
}
#endif
#ifdef USE_CLIMATE
void APIBLEServer::on_climate_update(climate::Climate *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_climate_state(obj);
}
#endif
#ifdef USE_NUMBER
void APIBLEServer::on_number_update(number::Number *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_number_state(obj);
}
#endif
#ifdef USE_SELECT
void APIBLEServer::on_select_update(select::Select *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_select_state(obj);
}
#endif
#ifdef USE_LOCK
void APIBLEServer::on_lock_update(lock::Lock *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_lock_state(obj);
}
#endif
#ifdef USE_MEDIA_PLAYER
void APIBLEServer::on_media_player_update(media_player::MediaPlayer *obj) {
  if (this->connection_ != nullptr && this->connection_->states_subscribed())
    this->connection_->send_media_player_state(obj);
}
#endif

void APIBLEServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "API BLE Server:\n"
                "  Service UUID: %s\n"
                "  Connection interval: %u-%u ms (latency %u, timeout %u ms)\n"
                "  Reboot timeout: %" PRIu32 " ms",
                SERVICE_UUID, this->conn_min_interval_ms_, this->conn_max_interval_ms_, this->conn_latency_,
                this->conn_timeout_ms_, this->reboot_timeout_);
#ifdef USE_API_BLE_NOISE
  ESP_LOGCONFIG(TAG, "  Encryption: Noise (PSK configured, plaintext rejected)");
#else
  ESP_LOGCONFIG(TAG, "  Encryption: none (plaintext)");
#endif
}

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
