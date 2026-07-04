#include "api_ble_server.h"

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

void APIBLEServer::setup_service_() {
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
      // handler. Single-central model: track the latest negotiated MTU.
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
    ESP_LOGW(TAG, "Second central connected (conn %u); ignoring (single-central model)", conn_id);
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
  this->close_session_();
  this->conn_id_ = INVALID_CONN_ID;
  this->mtu_ = DEFAULT_MTU;
  this->congested_ = false;
}

void APIBLEServer::open_session_(uint16_t conn_id) {
  if (this->connection_ != nullptr)
    return;  // already open (e.g. CCCD rewritten)
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
  }
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
