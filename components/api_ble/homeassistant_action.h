#pragma once

#include "esphome/core/defines.h"

#ifdef USE_API_BLE
#ifdef USE_ESP32

#include "api_ble_server.h"
#include "api_pb2.h"

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

/// YAML action `homeassistant.action` (alias `homeassistant.service`) and
/// `homeassistant.event`. Builds a HomeassistantActionRequest and forwards it
/// to the connected, service-subscribed client via APIBLEServer.
///
/// Phase 2 scope: no capture_response / on_success / on_error support (the
/// Python schema rejects those keys).
template<typename... Ts> class HomeAssistantServiceCallAction : public Action<Ts...> {
 public:
  explicit HomeAssistantServiceCallAction(APIBLEServer *parent, bool is_event) : parent_(parent), is_event_(is_event) {}

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

  void play(const Ts &...x) override {
    api::HomeassistantActionRequest resp;
    std::string service_value = this->service_.value(x...);
    resp.service = StringRef(service_value);
    resp.is_event = this->is_event_;

    // Local storage for lambda-evaluated strings - lives until after send.
    FixedVector<std::string> data_storage;
    FixedVector<std::string> data_template_storage;
    FixedVector<std::string> variables_storage;

    this->populate_service_map(resp.data, this->data_, data_storage, x...);
    this->populate_service_map(resp.data_template, this->data_template_, data_template_storage, x...);
    this->populate_service_map(resp.variables, this->variables_, variables_storage, x...);

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
  bool is_event_;
  TemplatableStringValue<Ts...> service_{};
  FixedVector<TemplatableKeyValuePair<Ts...>> data_;
  FixedVector<TemplatableKeyValuePair<Ts...>> data_template_;
  FixedVector<TemplatableKeyValuePair<Ts...>> variables_;
};

}  // namespace esphome::api_ble

#endif  // USE_ESP32
#endif  // USE_API_BLE
