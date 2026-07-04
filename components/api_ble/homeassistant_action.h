#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "api_ble_server.h"
#include "api_pb2.h"

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
#include "esphome/components/json/json_util.h"
#endif
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"

#include <utility>

namespace esphome::api_ble {

// Templatable string value that converts non-string lambda returns via
// to_string (ported from the in-tree api HomeAssistantServiceCallAction).
template<typename... X> class TemplatableStringValue : public TemplatableValue<std::string, X...> {
  static_assert(std::is_constructible_v<TemplatableValue<std::string, X...>, const char *>,
                "Base class must have const char* constructor for STATIC_STRING optimization");

 private:
  template<typename T> static std::string value_to_string(T &&val) {
    return to_string(std::forward<T>(val));  // NOLINT
  }
  static std::string value_to_string(char *val) { return val ? std::string(val) : std::string(); }
  static std::string value_to_string(const char *val) { return std::string(val); }
  static std::string value_to_string(const std::string &val) { return val; }
  static std::string value_to_string(std::string &&val) { return std::move(val); }
  static std::string value_to_string(const StringRef &val) { return val.str(); }
  static std::string value_to_string(StringRef &&val) { return val.str(); }

 public:
  TemplatableStringValue() : TemplatableValue<std::string, X...>() {}

  template<typename F, enable_if_t<!is_invocable<F, X...>::value, int> = 0>
  TemplatableStringValue(F value) : TemplatableValue<std::string, X...>(value) {}

  template<typename F, enable_if_t<is_invocable<F, X...>::value, int> = 0>
  TemplatableStringValue(F f)
      : TemplatableValue<std::string, X...>([f](X... x) -> std::string { return value_to_string(f(x...)); }) {}
};

template<typename... Ts> class TemplatableKeyValuePair {
 public:
  TemplatableKeyValuePair() = default;
  // Keys are always string literals from YAML dictionary keys (const char*,
  // remains in flash); only the value can be templatable.
  template<typename T> TemplatableKeyValuePair(const char *key, T value) : key(key), value(value) {}

  const char *key{nullptr};
  TemplatableStringValue<Ts...> value;
};

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
// Represents the response data from a Home Assistant action (ported from the
// in-tree api ActionResponse). Holds a StringRef into the error_message of the
// HomeassistantActionResponse protobuf message; that message must outlive the
// ActionResponse, which is guaranteed because the callback runs synchronously
// while the decoded message is still on the stack.
class ActionResponse {
 public:
  ActionResponse(bool success, StringRef error_message) : success_(success), error_message_(error_message) {}

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  ActionResponse(bool success, StringRef error_message, const uint8_t *data, size_t data_len)
      : success_(success), error_message_(error_message) {
    if (data == nullptr || data_len == 0)
      return;
    JsonDocument tmp = json::parse_json(data, data_len);
    swap(this->json_document_, tmp);
  }
#endif

  bool is_success() const { return this->success_; }
  const StringRef &get_error_message() const { return this->error_message_; }

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  JsonObjectConst get_json() const { return this->json_document_.as<JsonObjectConst>(); }
#endif

 protected:
  bool success_;
  StringRef error_message_;
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  JsonDocument json_document_;
#endif
};
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

/// YAML action `homeassistant.action` (alias `homeassistant.service`) and
/// `homeassistant.event`. Builds a HomeassistantActionRequest and forwards it
/// to the connected, service-subscribed client via APIBLEServer.
///
/// With `on_success` / `on_error` configured the action opts into response
/// tracking (mirrors the in-tree api component): a per-call `call_id` is sent
/// with the request and matched against the HomeassistantActionResponse the
/// client returns; `capture_response: true` additionally requests the JSON
/// response body and fires the success trigger with the parsed object.
template<typename... Ts> class HomeAssistantServiceCallAction : public Action<Ts...> {
 public:
  explicit HomeAssistantServiceCallAction(APIBLEServer *parent, bool is_event) : parent_(parent) {
    this->flags_.is_event = is_event;
  }

  template<typename T> void set_service(T service) { this->service_ = service; }

  void init_data(size_t count) { this->data_.init(count); }
  void init_data_template(size_t count) { this->data_template_.init(count); }
  void init_variables(size_t count) { this->variables_.init(count); }

  template<typename V> void add_data(const char *key, V &&value) {
    this->add_kv_(this->data_, key, std::forward<V>(value));
  }
  template<typename V> void add_data_template(const char *key, V &&value) {
    this->add_kv_(this->data_template_, key, std::forward<V>(value));
  }
  template<typename V> void add_variable(const char *key, V &&value) {
    this->add_kv_(this->variables_, key, std::forward<V>(value));
  }

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
  template<typename T> void set_response_template(T response_template) {
    this->response_template_ = response_template;
    this->flags_.has_response_template = true;
  }

  void set_wants_status() { this->flags_.wants_status = true; }
  void set_wants_response() { this->flags_.wants_response = true; }

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  Trigger<JsonObjectConst, Ts...> *get_success_trigger_with_response() { return &this->success_trigger_with_response_; }
#endif
  Trigger<Ts...> *get_success_trigger() { return &this->success_trigger_; }
  Trigger<std::string, Ts...> *get_error_trigger() { return &this->error_trigger_; }
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

  void play(const Ts &...x) override {
    api::HomeassistantActionRequest resp;
    std::string service_value = this->service_.value(x...);
    resp.service = StringRef(service_value);
    resp.is_event = this->flags_.is_event;

    // Local storage for lambda-evaluated strings - lives until after send.
    FixedVector<std::string> data_storage;
    FixedVector<std::string> data_template_storage;
    FixedVector<std::string> variables_storage;

    this->populate_service_map(resp.data, this->data_, data_storage, x...);
    this->populate_service_map(resp.data_template, this->data_template_, data_template_storage, x...);
    this->populate_service_map(resp.variables, this->variables_, variables_storage, x...);

#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
    // Declared at outer scope so it outlives send_homeassistant_action.
    std::string response_template_value;
#endif
    if (this->flags_.wants_status) {
      // Generate a unique call ID so the returned response can be matched back.
      static uint32_t call_id_counter = 1;
      uint32_t call_id = call_id_counter++;
      resp.call_id = call_id;
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
      if (this->flags_.wants_response) {
        resp.wants_response = true;
        if (this->flags_.has_response_template) {
          response_template_value = this->response_template_.value(x...);
          resp.response_template = StringRef(response_template_value);
        }
      }
#endif

      auto captured_args = std::make_tuple(x...);
      this->parent_->register_action_response_callback(call_id, [this, captured_args](const ActionResponse &response) {
        std::apply(
            [this, &response](auto &&...args) {
              if (response.is_success()) {
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
                if (this->flags_.wants_response) {
                  this->success_trigger_with_response_.trigger(response.get_json(), args...);
                } else
#endif
                {
                  this->success_trigger_.trigger(args...);
                }
              } else {
                this->error_trigger_.trigger(response.get_error_message(), args...);
              }
            },
            captured_args);
      });
    }
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

    this->parent_->send_homeassistant_action(resp);
  }

 protected:
  template<typename V> void add_kv_(FixedVector<TemplatableKeyValuePair<Ts...>> &vec, const char *key, V &&value) {
    auto &kv = vec.emplace_back();
    kv.key = key;
    kv.value = std::forward<V>(value);
  }

  template<typename VectorType, typename SourceType>
  static void populate_service_map(VectorType &dest, SourceType &source, FixedVector<std::string> &value_storage,
                                   Ts... x) {
    dest.init(source.size());
    // Count lambdas to size the backing storage exactly.
    size_t lambda_count = 0;
    for (const auto &it : source) {
      if (!it.value.is_static_string())
        lambda_count++;
    }
    value_storage.init(lambda_count);

    for (auto &it : source) {
      auto &kv = dest.emplace_back();
      kv.key = StringRef(it.key);
      if (it.value.is_static_string()) {
        kv.value = StringRef(it.value.get_static_string());
      } else {
        value_storage.push_back(it.value.value(x...));
        kv.value = StringRef(value_storage.back());
      }
    }
  }

  APIBLEServer *parent_;
  TemplatableStringValue<Ts...> service_{};
  FixedVector<TemplatableKeyValuePair<Ts...>> data_;
  FixedVector<TemplatableKeyValuePair<Ts...>> data_template_;
  FixedVector<TemplatableKeyValuePair<Ts...>> variables_;
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES
#ifdef USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  TemplatableStringValue<Ts...> response_template_{""};
  Trigger<JsonObjectConst, Ts...> success_trigger_with_response_;
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON
  Trigger<Ts...> success_trigger_;
  Trigger<std::string, Ts...> error_trigger_;
#endif  // USE_API_HOMEASSISTANT_ACTION_RESPONSES

  struct Flags {
    uint8_t is_event : 1;
    uint8_t wants_status : 1;
    uint8_t wants_response : 1;
    uint8_t has_response_template : 1;
    uint8_t reserved : 4;
  } flags_{};
};

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
