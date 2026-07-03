#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "ble_byte_pipe.h"
#include "proto.h"
#include "api_pb2.h"

#include <cstdint>
#include <string>

// Entity type headers for the supported subset. Gated by the USE_* defines so
// the connection compiles whether or not a given platform is present.
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_MEDIA_PLAYER
#include "esphome/components/media_player/media_player.h"
#endif

namespace esphome::api_ble {

class APIBLEServer;

/// Keepalive: mirror the api component's timings (api_connection.h/.cpp).
static constexpr uint32_t KEEPALIVE_TIMEOUT_MS = 60000;
static constexpr uint32_t KEEPALIVE_DISCONNECT_TIMEOUT_MS = (KEEPALIVE_TIMEOUT_MS * 5) / 2;

/// Native API connection state machine over a BLEBytePipe.
///
/// Plaintext frames only (indicator 0x00 + varint size + varint type). Noise
/// framing (indicator 0x01) is phase 3, gated by USE_API_BLE_NOISE.
class APIBLEConnection {
 public:
  explicit APIBLEConnection(APIBLEServer *server);

  /// Called from APIBLEServer::loop(): parse buffered RX frames and run
  /// keepalive bookkeeping. Does not drain TX (the server does that so the
  /// drain also covers the just-produced responses).
  void loop();

  BLEBytePipe &pipe() { return this->pipe_; }

  /// True once a fatal protocol error, DisconnectRequest round-trip or
  /// keepalive expiry requires the server to tear down this session (and the
  /// BLE link).
  bool should_close() const { return this->should_close_; }
  /// True only after the client's DisconnectRequest was answered -- the link
  /// teardown must wait until the DisconnectResponse has been notified out.
  bool close_after_flush() const { return this->close_after_flush_; }

  /// True once the client has sent SubscribeStatesRequest.
  bool states_subscribed() const { return this->states_subscribed_; }

  /// Encode a protobuf message into the pipe with a plaintext frame header.
  /// Template because ProtoMessage::encode()/calculate_size() are resolved
  /// statically (non-virtual) in the vendored proto layer.
  template<typename Msg> bool send_message(const Msg &msg) {
    uint32_t payload_size = msg.calculate_size();
    this->tx_buf_.clear();
    this->tx_buf_.resize(FRAME_HEADER_PADDING + payload_size);
    api::ProtoWriteBuffer buffer(&this->tx_buf_, FRAME_HEADER_PADDING);
    msg.encode(buffer PROTO_ENCODE_DEBUG_INIT(&this->tx_buf_));
    return this->write_frame_(Msg::MESSAGE_TYPE, payload_size);
  }

  // Push-on-change state entrypoints, called by APIBLEServer from the
  // Controller update callbacks. Each is a no-op when the type isn't built.
#ifdef USE_BINARY_SENSOR
  bool send_binary_sensor_state(binary_sensor::BinarySensor *entity);
#endif
#ifdef USE_SENSOR
  bool send_sensor_state(sensor::Sensor *entity);
#endif
#ifdef USE_TEXT_SENSOR
  bool send_text_sensor_state(text_sensor::TextSensor *entity);
#endif
#ifdef USE_SWITCH
  bool send_switch_state(switch_::Switch *entity);
#endif
#ifdef USE_LIGHT
  bool send_light_state(light::LightState *entity);
#endif
#ifdef USE_MEDIA_PLAYER
  bool send_media_player_state(media_player::MediaPlayer *entity);
#endif

  /// Forward a HomeassistantActionRequest to the client, only when it has
  /// subscribed to Home Assistant services (same drop-otherwise semantics as
  /// the in-tree api component).
  void send_homeassistant_action(const api::HomeassistantActionRequest &req);

 protected:
  /// Max plaintext header: 1 (indicator) + up to 3 (varint16 size) + up to 2
  /// (varint8 type). Same layout idea as the api component's HEADER_PADDING;
  /// the header is right-justified so it sits immediately before the payload.
  static constexpr size_t FRAME_HEADER_PADDING = 6;
  /// Reject frames larger than this (matches the intent of the api
  /// component's rx limits; BLE MVP messages are far smaller).
  static constexpr uint32_t MAX_RX_FRAME_SIZE = 8192;

  void parse_frames_();
  void dispatch_message_(uint32_t type, const uint8_t *data, uint32_t len);
  void check_keepalive_(uint32_t now);
  void fatal_error_(const char *reason);
  bool write_frame_(uint8_t message_type, uint16_t payload_size);

  bool client_supports_api_version_(uint16_t major, uint16_t minor) const {
    return this->client_api_version_major_ > major ||
           (this->client_api_version_major_ == major && this->client_api_version_minor_ >= minor);
  }

  // Fill the base InfoResponseProtoMessage fields shared by all entity types,
  // then encode+queue the message. The caller must keep any StringRef-backing
  // buffers (e.g. device_class) alive for the duration of the call; encoding
  // happens synchronously inside send_message.
  template<typename Msg> bool send_entity_info_(esphome::EntityBase *entity, Msg &msg) {
    msg.key = entity->get_object_id_hash();
    char object_id_buf[esphome::OBJECT_ID_MAX_LEN];
    if (!this->client_supports_api_version_(1, 14)) {
      msg.object_id = entity->get_object_id_to(object_id_buf);
    }
    if (entity->has_own_name()) {
      msg.name = entity->get_name();
    }
#ifdef USE_ENTITY_ICON
    char icon_buf[esphome::MAX_ICON_LENGTH];
    msg.icon = api::StringRef(entity->get_icon_to(icon_buf));
#endif
    msg.disabled_by_default = entity->is_disabled_by_default();
    msg.entity_category = static_cast<api::enums::EntityCategory>(entity->get_entity_category());
    return this->send_message(msg);
  }

  template<typename Msg> bool send_entity_state_(esphome::EntityBase *entity, Msg &msg) {
    msg.key = entity->get_object_id_hash();
    return this->send_message(msg);
  }

  // Message handlers
  void on_hello_request_(const uint8_t *data, uint32_t len);
  void on_device_info_request_();
  void on_ping_request_();
  void on_ping_response_();
  void on_disconnect_request_();
  void on_list_entities_request_();
  void on_subscribe_states_request_();
  void on_subscribe_homeassistant_services_request_();
  void on_subscribe_home_assistant_states_request_();
  void on_home_assistant_state_response_(const uint8_t *data, uint32_t len);
#ifdef USE_SWITCH
  void on_switch_command_request_(const uint8_t *data, uint32_t len);
#endif
#ifdef USE_LIGHT
  void on_light_command_request_(const uint8_t *data, uint32_t len);
#endif
#ifdef USE_MEDIA_PLAYER
  void on_media_player_command_request_(const uint8_t *data, uint32_t len);
#endif

  APIBLEServer *server_;
  BLEBytePipe pipe_;
  api::APIBuffer tx_buf_;

  std::string client_info_;
  uint32_t last_traffic_ms_{0};
  uint16_t client_api_version_major_{0};
  uint16_t client_api_version_minor_{0};
  bool sent_ping_{false};
  bool connected_{false};  // HelloRequest/HelloResponse handshake done
  bool states_subscribed_{false};
  bool service_call_subscription_{false};
  bool should_close_{false};
  bool close_after_flush_{false};
};

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
