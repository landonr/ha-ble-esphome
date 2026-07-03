#include "api_ble_connection.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "api_ble_server.h"
#include "api_pb2.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include <cinttypes>

namespace esphome::api_ble {

static const char *const TAG = "api_ble.connection";

static constexpr auto ESPHOME_VERSION_REF = StringRef::from_lit(ESPHOME_VERSION);

using namespace api;  // vendored proto layer (see PROTO_VENDORED.md)

APIBLEConnection::APIBLEConnection(APIBLEServer *server) : server_(server) {
  this->last_traffic_ms_ = millis();
  ESP_LOGD(TAG, "Session opened (CCCD subscription)");
}

void APIBLEConnection::loop() {
  if (this->should_close_)
    return;

  this->parse_frames_();

  const uint32_t now = millis();
  // When sent_ping_ is true, last_traffic_ms_ hasn't been updated, so this
  // condition covers both the send-ping and the disconnect case (mirrors
  // api_connection.cpp).
  if (now - this->last_traffic_ms_ > KEEPALIVE_TIMEOUT_MS) {
    this->check_keepalive_(now);
  }
}

void APIBLEConnection::check_keepalive_(uint32_t now) {
  if (this->sent_ping_) {
    // Drop the session if the client did not answer within 2.5x keepalive.
    if (now - this->last_traffic_ms_ > KEEPALIVE_DISCONNECT_TIMEOUT_MS) {
      this->fatal_error_("ping timeout");
    }
    return;
  }
  ESP_LOGVV(TAG, "Sending keepalive PING");
  PingRequest req;
  // Mark as sent even if the TX ring rejected it, to avoid re-queueing every
  // loop; the 2.5x window still applies (same approach as api_connection).
  this->send_message(req);
  this->sent_ping_ = true;
}

void APIBLEConnection::fatal_error_(const char *reason) {
  ESP_LOGW(TAG, "Closing session: %s", reason);
  this->should_close_ = true;
}

void APIBLEConnection::parse_frames_() {
  while (!this->should_close_ && this->pipe_.rx_size() > 0) {
    const uint8_t *data = this->pipe_.rx_data();
    const uint32_t avail = this->pipe_.rx_size();

    if (data[0] != 0x00) {
      // TODO(phase 3, USE_API_BLE_NOISE): 0x01 introduces a Noise frame.
      this->fatal_error_("bad frame indicator (expected plaintext 0x00)");
      this->pipe_.reset();
      return;
    }
    if (avail < 3)
      return;  // indicator + at least 1 byte size + 1 byte type

    uint32_t offset = 1;
    auto size_res = ProtoVarInt::parse(data + offset, avail - offset);
    if (!size_res.has_value()) {
      if (avail - offset >= 5) {
        this->fatal_error_("invalid frame size varint");
        this->pipe_.reset();
      }
      return;  // otherwise: incomplete varint, wait for more bytes
    }
    offset += size_res.consumed;

    auto type_res = ProtoVarInt::parse(data + offset, avail - offset);
    if (!type_res.has_value()) {
      if (avail - offset >= 5) {
        this->fatal_error_("invalid frame type varint");
        this->pipe_.reset();
      }
      return;
    }
    offset += type_res.consumed;

    const uint32_t payload_size = static_cast<uint32_t>(size_res.value);
    const uint32_t msg_type = static_cast<uint32_t>(type_res.value);
    if (payload_size > MAX_RX_FRAME_SIZE) {
      this->fatal_error_("frame too large");
      this->pipe_.reset();
      return;
    }
    if (avail - offset < payload_size)
      return;  // frame spans more GATT packets, wait

    this->last_traffic_ms_ = millis();
    this->dispatch_message_(msg_type, data + offset, payload_size);
    this->pipe_.rx_consume(offset + payload_size);
  }
}

void APIBLEConnection::dispatch_message_(uint32_t type, const uint8_t *data, uint32_t len) {
  ESP_LOGVV(TAG, "RX message type=%" PRIu32 " len=%" PRIu32, type, len);
  switch (type) {
    case HelloRequest::MESSAGE_TYPE:  // 1
      this->on_hello_request_(data, len);
      break;
    case DisconnectRequest::MESSAGE_TYPE:  // 5
      this->on_disconnect_request_();
      break;
    case DisconnectResponse::MESSAGE_TYPE:  // 6: answer to a server-initiated disconnect
      this->should_close_ = true;
      break;
    case PingRequest::MESSAGE_TYPE:  // 7
      this->on_ping_request_();
      break;
    case PingResponse::MESSAGE_TYPE:  // 8
      this->on_ping_response_();
      break;
    case 9:  // DeviceInfoRequest (empty message, no generated class)
      this->on_device_info_request_();
      break;
    case 11:  // ListEntitiesRequest (empty message)
      this->on_list_entities_request_();
      break;
    case 20:  // SubscribeStatesRequest (empty message)
      this->on_subscribe_states_request_();
      break;
#ifdef USE_LIGHT
    case 32:  // LightCommandRequest
      this->on_light_command_request_(data, len);
      break;
#endif
#ifdef USE_SWITCH
    case 33:  // SwitchCommandRequest
      this->on_switch_command_request_(data, len);
      break;
#endif
    case 34:  // SubscribeHomeassistantServicesRequest (empty message)
      this->on_subscribe_homeassistant_services_request_();
      break;
    case 38:  // SubscribeHomeAssistantStatesRequest (empty message)
      this->on_subscribe_home_assistant_states_request_();
      break;
    case HomeAssistantStateResponse::MESSAGE_TYPE:  // 40
      this->on_home_assistant_state_response_(data, len);
      break;
#ifdef USE_MEDIA_PLAYER
    case 65:  // MediaPlayerCommandRequest
      this->on_media_player_command_request_(data, len);
      break;
#endif
    default:
      ESP_LOGW(TAG, "Unhandled message type %" PRIu32 " (len=%" PRIu32 ")", type, len);
      break;
  }
}

void APIBLEConnection::on_hello_request_(const uint8_t *data, uint32_t len) {
  HelloRequest msg;
  msg.decode(data, len);
  this->client_api_version_major_ = msg.api_version_major;
  this->client_api_version_minor_ = msg.api_version_minor;
  this->client_info_ = msg.client_info.str();
  ESP_LOGV(TAG, "Hello from client: '%s' | API Version %u.%u", this->client_info_.c_str(),
           this->client_api_version_major_, this->client_api_version_minor_);

  // Same version the api component sends today (api_connection.cpp).
  HelloResponse resp;
  resp.api_version_major = 1;
  resp.api_version_minor = 14;
  resp.server_info = ESPHOME_VERSION_REF;
  resp.name = StringRef(App.get_name());
  if (this->send_message(resp)) {
    this->connected_ = true;
    // Keepalive starts from the handshake, not the BLE connect.
    this->last_traffic_ms_ = millis();
    this->server_->notify_client_hello(this->client_info_);
  }
}

void APIBLEConnection::on_device_info_request_() {
  DeviceInfoResponse resp;
  resp.name = StringRef(App.get_name());
  resp.friendly_name = StringRef(App.get_friendly_name());

  // Stack buffer for MAC address (XX:XX:XX:XX:XX:XX\0 = 18 bytes); valid
  // until send_message returns, which encodes within this scope.
  char mac_address[18];
  uint8_t mac[6];
  get_mac_address_raw(mac);
  format_mac_addr_upper(mac, mac_address);
  resp.mac_address = StringRef(mac_address);

  resp.esphome_version = ESPHOME_VERSION_REF;

  char build_time_str[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_time_str);
  resp.compilation_time = StringRef(build_time_str);

  static constexpr auto MANUFACTURER = StringRef::from_lit("Espressif");
  resp.manufacturer = MANUFACTURER;
  static constexpr auto MODEL = StringRef::from_lit(ESPHOME_BOARD);
  resp.model = MODEL;

  this->send_message(resp);
}

void APIBLEConnection::on_ping_request_() {
  PingResponse resp;
  this->send_message(resp);
}

void APIBLEConnection::on_ping_response_() {
  // We initiated the ping; traffic timer was already refreshed in
  // parse_frames_(), just clear the outstanding flag.
  this->sent_ping_ = false;
}

void APIBLEConnection::on_disconnect_request_() {
  ESP_LOGD(TAG, "Client requested disconnect");
  DisconnectResponse resp;
  this->send_message(resp);
  // Tear down only after the response has drained out of the TX ring.
  this->should_close_ = true;
  this->close_after_flush_ = true;
}

// ---------------------------------------------------------------------------
// ListEntities
// ---------------------------------------------------------------------------

void APIBLEConnection::on_list_entities_request_() {
  // Enumerate the supported entity subset synchronously into the TX ring.
  // send_* returns false when the ring overflows (write_frame_ closes the
  // session); bail out so the server tears the link down gracefully.
#ifdef USE_BINARY_SENSOR
  for (auto *entity : App.get_binary_sensors()) {
    if (entity->is_internal())
      continue;
    ListEntitiesBinarySensorResponse msg;
    msg.is_status_binary_sensor = entity->is_status_binary_sensor();
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    msg.device_class = StringRef(entity->get_device_class_to(dc_buf));
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif
#ifdef USE_SENSOR
  for (auto *entity : App.get_sensors()) {
    if (entity->is_internal())
      continue;
    ListEntitiesSensorResponse msg;
    msg.unit_of_measurement = entity->get_unit_of_measurement_ref();
    msg.accuracy_decimals = entity->get_accuracy_decimals();
    msg.force_update = entity->get_force_update();
    msg.state_class = static_cast<enums::SensorStateClass>(entity->get_state_class());
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    msg.device_class = StringRef(entity->get_device_class_to(dc_buf));
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *entity : App.get_text_sensors()) {
    if (entity->is_internal())
      continue;
    ListEntitiesTextSensorResponse msg;
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    msg.device_class = StringRef(entity->get_device_class_to(dc_buf));
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif
#ifdef USE_SWITCH
  for (auto *entity : App.get_switches()) {
    if (entity->is_internal())
      continue;
    ListEntitiesSwitchResponse msg;
    msg.assumed_state = entity->assumed_state();
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    msg.device_class = StringRef(entity->get_device_class_to(dc_buf));
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif
#ifdef USE_LIGHT
  for (auto *entity : App.get_lights()) {
    if (entity->is_internal())
      continue;
    ListEntitiesLightResponse msg;
    auto traits = entity->get_traits();
    auto supported_modes = traits.get_supported_color_modes();
    msg.supported_color_modes = &supported_modes;
    if (traits.supports_color_capability(light::ColorCapability::COLOR_TEMPERATURE) ||
        traits.supports_color_capability(light::ColorCapability::COLD_WARM_WHITE)) {
      msg.min_mireds = traits.get_min_mireds();
      msg.max_mireds = traits.get_max_mireds();
    }
    FixedVector<const char *> effects_list;
    if (entity->supports_effects()) {
      auto &light_effects = entity->get_effects();
      effects_list.init(light_effects.size() + 1);
      effects_list.push_back("None");
      for (auto *effect : light_effects) {
        effects_list.push_back(effect->get_name().c_str());
      }
    }
    msg.effects = &effects_list;
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif
#ifdef USE_MEDIA_PLAYER
  for (auto *entity : App.get_media_players()) {
    if (entity->is_internal())
      continue;
    ListEntitiesMediaPlayerResponse msg;
    auto traits = entity->get_traits();
    msg.supports_pause = traits.get_supports_pause();
    msg.feature_flags = traits.get_feature_flags();
    for (auto &supported_format : traits.get_supported_formats()) {
      msg.supported_formats.emplace_back();
      auto &media_format = msg.supported_formats.back();
      media_format.format = StringRef(supported_format.format);
      media_format.sample_rate = supported_format.sample_rate;
      media_format.num_channels = supported_format.num_channels;
      media_format.purpose = static_cast<enums::MediaPlayerFormatPurpose>(supported_format.purpose);
      media_format.sample_bytes = supported_format.sample_bytes;
    }
    if (!this->send_entity_info_(entity, msg))
      return;
  }
#endif

  ListEntitiesDoneResponse done;
  this->send_message(done);
}

// ---------------------------------------------------------------------------
// SubscribeStates + push-on-change state encoders
// ---------------------------------------------------------------------------

void APIBLEConnection::on_subscribe_states_request_() {
  this->states_subscribed_ = true;
  // Initial state dump: only entities that already have a state.
#ifdef USE_BINARY_SENSOR
  for (auto *entity : App.get_binary_sensors()) {
    if (entity->is_internal())
      continue;
    if (!this->send_binary_sensor_state(entity))
      return;
  }
#endif
#ifdef USE_SENSOR
  for (auto *entity : App.get_sensors()) {
    if (entity->is_internal())
      continue;
    if (!this->send_sensor_state(entity))
      return;
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *entity : App.get_text_sensors()) {
    if (entity->is_internal())
      continue;
    if (!this->send_text_sensor_state(entity))
      return;
  }
#endif
#ifdef USE_SWITCH
  for (auto *entity : App.get_switches()) {
    if (entity->is_internal())
      continue;
    if (!this->send_switch_state(entity))
      return;
  }
#endif
#ifdef USE_LIGHT
  for (auto *entity : App.get_lights()) {
    if (entity->is_internal())
      continue;
    if (!this->send_light_state(entity))
      return;
  }
#endif
#ifdef USE_MEDIA_PLAYER
  for (auto *entity : App.get_media_players()) {
    if (entity->is_internal())
      continue;
    if (!this->send_media_player_state(entity))
      return;
  }
#endif
}

#ifdef USE_BINARY_SENSOR
bool APIBLEConnection::send_binary_sensor_state(binary_sensor::BinarySensor *entity) {
  BinarySensorStateResponse resp;
  resp.state = entity->state;
  resp.missing_state = !entity->has_state();
  return this->send_entity_state_(entity, resp);
}
#endif

#ifdef USE_SENSOR
bool APIBLEConnection::send_sensor_state(sensor::Sensor *entity) {
  SensorStateResponse resp;
  resp.state = entity->state;
  resp.missing_state = !entity->has_state();
  return this->send_entity_state_(entity, resp);
}
#endif

#ifdef USE_TEXT_SENSOR
bool APIBLEConnection::send_text_sensor_state(text_sensor::TextSensor *entity) {
  TextSensorStateResponse resp;
  resp.state = StringRef(entity->state);
  resp.missing_state = !entity->has_state();
  return this->send_entity_state_(entity, resp);
}
#endif

#ifdef USE_SWITCH
bool APIBLEConnection::send_switch_state(switch_::Switch *entity) {
  SwitchStateResponse resp;
  resp.state = entity->state;
  return this->send_entity_state_(entity, resp);
}
#endif

#ifdef USE_LIGHT
bool APIBLEConnection::send_light_state(light::LightState *entity) {
  LightStateResponse resp;
  auto values = entity->remote_values;
  auto color_mode = values.get_color_mode();
  resp.state = values.is_on();
  resp.color_mode = static_cast<enums::ColorMode>(color_mode);
  resp.brightness = values.get_brightness();
  resp.color_brightness = values.get_color_brightness();
  resp.red = values.get_red();
  resp.green = values.get_green();
  resp.blue = values.get_blue();
  resp.white = values.get_white();
  resp.color_temperature = values.get_color_temperature();
  resp.cold_white = values.get_cold_white();
  resp.warm_white = values.get_warm_white();
  if (entity->supports_effects()) {
    resp.effect = entity->get_effect_name();
  }
  return this->send_entity_state_(entity, resp);
}
#endif

#ifdef USE_MEDIA_PLAYER
bool APIBLEConnection::send_media_player_state(media_player::MediaPlayer *entity) {
  MediaPlayerStateResponse resp;
  media_player::MediaPlayerState report_state = entity->state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING
                                                    ? media_player::MEDIA_PLAYER_STATE_PLAYING
                                                    : entity->state;
  resp.state = static_cast<enums::MediaPlayerState>(report_state);
  resp.volume = entity->volume;
  resp.muted = entity->is_muted();
  return this->send_entity_state_(entity, resp);
}
#endif

// ---------------------------------------------------------------------------
// Home Assistant services (device -> HA action forwarding)
// ---------------------------------------------------------------------------

void APIBLEConnection::on_subscribe_homeassistant_services_request_() {
  ESP_LOGD(TAG, "Client subscribed to Home Assistant services");
  this->service_call_subscription_ = true;
}

void APIBLEConnection::send_homeassistant_action(const HomeassistantActionRequest &req) {
  if (!this->service_call_subscription_)
    return;
  this->send_message(req);
}

// ---------------------------------------------------------------------------
// Home Assistant state import (HA -> device)
// ---------------------------------------------------------------------------

void APIBLEConnection::on_subscribe_home_assistant_states_request_() {
  // Reply with one SubscribeHomeAssistantStateResponse per registered
  // subscription. Small counts; encode synchronously into the TX ring.
  for (const auto &sub : this->server_->get_state_subs()) {
    SubscribeHomeAssistantStateResponse resp;
    resp.entity_id = StringRef(sub.entity_id);
    resp.attribute = sub.attribute != nullptr ? StringRef(sub.attribute) : StringRef("");
    resp.once = sub.once;
    if (!this->send_message(resp))
      return;
  }
}

void APIBLEConnection::on_home_assistant_state_response_(const uint8_t *data, uint32_t len) {
  HomeAssistantStateResponse msg;
  msg.decode(data, len);
  if (msg.entity_id.empty())
    return;

  // Copy into null-terminated std::strings so callbacks (parse_number, etc.)
  // can safely use c_str(); the StringRefs point into the transient RX buffer.
  std::string entity_id = msg.entity_id.str();
  std::string state = msg.state.str();
  std::string attribute = msg.attribute.str();
  StringRef state_ref(state);
  bool msg_has_attribute = !msg.attribute.empty();

  for (const auto &sub : this->server_->get_state_subs()) {
    if (entity_id != sub.entity_id)
      continue;
    // Subscriber with attribute filter: message attribute must match it.
    // Subscriber without filter: message must carry no attribute.
    if (sub.attribute != nullptr) {
      if (attribute != sub.attribute)
        continue;
    } else if (msg_has_attribute) {
      continue;
    }
    sub.callback(state_ref);
  }
}

// ---------------------------------------------------------------------------
// Entity command handlers
// ---------------------------------------------------------------------------

#ifdef USE_SWITCH
void APIBLEConnection::on_switch_command_request_(const uint8_t *data, uint32_t len) {
  SwitchCommandRequest msg;
  msg.decode(data, len);
#ifdef USE_DEVICES
  auto *entity = App.get_switch_by_key(msg.key, msg.device_id);
#else
  auto *entity = App.get_switch_by_key(msg.key);
#endif
  if (entity == nullptr) {
    ESP_LOGW(TAG, "SwitchCommandRequest: unknown key %" PRIu32, msg.key);
    return;
  }
  if (msg.state) {
    entity->turn_on();
  } else {
    entity->turn_off();
  }
}
#endif

#ifdef USE_LIGHT
void APIBLEConnection::on_light_command_request_(const uint8_t *data, uint32_t len) {
  LightCommandRequest msg;
  msg.decode(data, len);
#ifdef USE_DEVICES
  auto *entity = App.get_light_by_key(msg.key, msg.device_id);
#else
  auto *entity = App.get_light_by_key(msg.key);
#endif
  if (entity == nullptr) {
    ESP_LOGW(TAG, "LightCommandRequest: unknown key %" PRIu32, msg.key);
    return;
  }
  auto call = entity->make_call();
  if (msg.has_state)
    call.set_state(msg.state);
  if (msg.has_brightness)
    call.set_brightness(msg.brightness);
  if (msg.has_color_mode)
    call.set_color_mode(static_cast<light::ColorMode>(msg.color_mode));
  if (msg.has_color_brightness)
    call.set_color_brightness(msg.color_brightness);
  if (msg.has_rgb) {
    call.set_red(msg.red);
    call.set_green(msg.green);
    call.set_blue(msg.blue);
  }
  if (msg.has_white)
    call.set_white(msg.white);
  if (msg.has_color_temperature)
    call.set_color_temperature(msg.color_temperature);
  if (msg.has_cold_white)
    call.set_cold_white(msg.cold_white);
  if (msg.has_warm_white)
    call.set_warm_white(msg.warm_white);
  if (msg.has_transition_length)
    call.set_transition_length(msg.transition_length);
  if (msg.has_flash_length)
    call.set_flash_length(msg.flash_length);
  if (msg.has_effect)
    call.set_effect(msg.effect.c_str(), msg.effect.size());
  call.perform();
}
#endif

#ifdef USE_MEDIA_PLAYER
void APIBLEConnection::on_media_player_command_request_(const uint8_t *data, uint32_t len) {
  MediaPlayerCommandRequest msg;
  msg.decode(data, len);
#ifdef USE_DEVICES
  auto *entity = App.get_media_player_by_key(msg.key, msg.device_id);
#else
  auto *entity = App.get_media_player_by_key(msg.key);
#endif
  if (entity == nullptr) {
    ESP_LOGW(TAG, "MediaPlayerCommandRequest: unknown key %" PRIu32, msg.key);
    return;
  }
  auto call = entity->make_call();
  if (msg.has_command)
    call.set_command(static_cast<media_player::MediaPlayerCommand>(msg.command));
  if (msg.has_volume)
    call.set_volume(msg.volume);
  if (msg.has_media_url)
    call.set_media_url(msg.media_url);
  if (msg.has_announcement)
    call.set_announcement(msg.announcement);
  call.perform();
}
#endif

bool APIBLEConnection::write_frame_(uint8_t message_type, uint16_t payload_size) {
  // Plaintext header, right-justified within the padding so header + payload
  // are contiguous (simplified version of the api component's
  // write_plaintext_header; correctness over cleverness).
  const uint8_t size_varint_len = ProtoSize::varint16(payload_size);
  const uint8_t type_varint_len = ProtoSize::varint8(message_type);
  const uint8_t header_len = 1 + size_varint_len + type_varint_len;
  const size_t header_offset = FRAME_HEADER_PADDING - header_len;

  uint8_t *buf = this->tx_buf_.data();
  buf[header_offset] = 0x00;  // plaintext indicator
  encode_varint_to_buffer(payload_size, buf + header_offset + 1);
  encode_varint_to_buffer(message_type, buf + header_offset + 1 + size_varint_len);

  if (!this->pipe_.write(buf + header_offset, header_len + payload_size)) {
    // Same semantics as the TCP helper's overflow buffer limit: overflow is
    // fatal for the session.
    this->fatal_error_("TX ring overflow");
    return false;
  }
  return true;
}

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
