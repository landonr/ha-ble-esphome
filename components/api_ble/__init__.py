"""ESPHome native API over BLE GATT (transport swap for the `api` component).

Self-contained: vendors the api component's proto layer (see PROTO_VENDORED.md)
and never loads the in-tree `api` component (CONFLICTS_WITH).
"""

import base64

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32_ble
from esphome.components.esp32_ble import BTLoggers
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTION,
    CONF_ATTRIBUTE,
    CONF_CAPTURE_RESPONSE,
    CONF_DATA,
    CONF_DATA_TEMPLATE,
    CONF_ENTITY_ID,
    CONF_EVENT,
    CONF_ID,
    CONF_INTERNAL,
    CONF_KEY,
    CONF_ON_CLIENT_CONNECTED,
    CONF_ON_CLIENT_DISCONNECTED,
    CONF_ON_ERROR,
    CONF_ON_SUCCESS,
    CONF_REBOOT_TIMEOUT,
    CONF_RESPONSE_TEMPLATE,
    CONF_SERVICE,
    CONF_TIMEOUT,
    CONF_VARIABLES,
)
from esphome.core import CORE

CODEOWNERS = ["@landonrohatensky"]  # TODO: placeholder
DEPENDENCIES = ["esp32", "esp32_ble_server"]
CONFLICTS_WITH = ["api"]

CONF_ENCRYPTION = "encryption"
CONF_CONNECTION = "connection"
CONF_MIN_INTERVAL = "min_interval"
CONF_MAX_INTERVAL = "max_interval"
CONF_LATENCY = "latency"

api_ble_ns = cg.esphome_ns.namespace("api_ble")
APIBLEServer = api_ble_ns.class_("APIBLEServer", cg.Component)
HomeAssistantServiceCallAction = api_ble_ns.class_(
    "HomeAssistantServiceCallAction", automation.Action
)


def validate_encryption_key(value):
    """Same validation as api.validate_encryption_key: base64, 32 bytes."""
    value = cv.string_strict(value)
    try:
        decoded = base64.b64decode(value, validate=True)
    except ValueError as err:
        raise cv.Invalid("Invalid key format, please check it's using base64") from err

    if len(decoded) != 32:
        raise cv.Invalid("Encryption key must be base64 and 32 bytes long")

    # Return original data for roundtrip conversion
    return value


ENCRYPTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_KEY): cv.sensitive(validate_encryption_key),
    }
)

CONNECTION_SCHEMA = cv.Schema(
    {
        cv.Optional(
            CONF_MIN_INTERVAL, default="30ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_MAX_INTERVAL, default="60ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_LATENCY, default=0): cv.uint16_t,
        cv.Optional(CONF_TIMEOUT, default="5s"): cv.positive_time_period_milliseconds,
    }
)


def _validate_connection(config):
    if config[CONF_MIN_INTERVAL] > config[CONF_MAX_INTERVAL]:
        raise cv.Invalid(
            f"min_interval ({config[CONF_MIN_INTERVAL]}) must be less than or "
            f"equal to max_interval ({config[CONF_MAX_INTERVAL]})"
        )
    return config


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(APIBLEServer),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Optional(CONF_ENCRYPTION): ENCRYPTION_SCHEMA,
        cv.Optional(
            CONF_REBOOT_TIMEOUT, default="15min"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CONNECTION, default={}): cv.All(
            CONNECTION_SCHEMA, _validate_connection
        ),
        cv.Optional(CONF_ON_CLIENT_CONNECTED): automation.validate_automation(
            single=True
        ),
        cv.Optional(CONF_ON_CLIENT_DISCONNECTED): automation.validate_automation(
            single=True
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # Register the BT loggers this component needs (mirrors esp32_ble_server)
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # APIBLEServer is a Controller (push-on-change state updates via the
    # ControllerRegistry, exactly like the in-tree api component).
    CORE.register_controller()

    # Register our own GATTS event handler with esp32_ble so the
    # StaticCallbackManager is sized to include us (same mechanism as
    # esp32_ble_server). We need raw GATTS events for ESP_GATTS_MTU_EVT
    # (MTU tracking), ESP_GATTS_CONNECT_EVT (peer address for
    # esp_ble_gap_update_conn_params) and CCCD writes (session-open signal).
    parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    esp32_ble.register_gatts_event_handler(parent, var)

    cg.add(var.set_reboot_timeout(config[CONF_REBOOT_TIMEOUT]))

    conn = config[CONF_CONNECTION]
    cg.add(
        var.set_connection_params(
            conn[CONF_MIN_INTERVAL],
            conn[CONF_MAX_INTERVAL],
            conn[CONF_LATENCY],
            conn[CONF_TIMEOUT],
        )
    )

    if CONF_ON_CLIENT_CONNECTED in config:
        cg.add_define("USE_API_BLE_CLIENT_CONNECTED_TRIGGER")
        await automation.build_automation(
            var.get_client_connected_trigger(),
            [(cg.std_string, "client_info"), (cg.std_string, "client_address")],
            config[CONF_ON_CLIENT_CONNECTED],
        )

    if CONF_ON_CLIENT_DISCONNECTED in config:
        cg.add_define("USE_API_BLE_CLIENT_DISCONNECTED_TRIGGER")
        await automation.build_automation(
            var.get_client_disconnected_trigger(),
            [(cg.std_string, "client_info"), (cg.std_string, "client_address")],
            config[CONF_ON_CLIENT_DISCONNECTED],
        )

    cg.add_define("USE_API_BLE")

    # Home Assistant services (device -> HA action forwarding) and HA state
    # import (HA -> device) are always compiled in: the vendored proto message
    # classes are gated by these defines, and the connection handles both
    # request paths unconditionally.
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_STATES")

    if (encryption_config := config.get(CONF_ENCRYPTION)) is not None:
        decoded = base64.b64decode(encryption_config[CONF_KEY])
        cg.add(var.set_noise_psk(list(decoded)))
        # With a PSK configured the connection requires Noise frames and
        # rejects plaintext clients (in-tree semantics). See noise_session.h.
        cg.add_define("USE_API_BLE_NOISE")
        cg.add_library("esphome/noise-c", "0.1.11")


# ---------------------------------------------------------------------------
# homeassistant.action / homeassistant.service / homeassistant.event
#
# Same YAML grammar as the in-tree api component. capture_response / on_success
# / on_error are NOT yet supported (phase 2) -- using them raises cv.Invalid.
# ---------------------------------------------------------------------------

KEY_VALUE_SCHEMA = cv.Schema({cv.string: cv.templatable(cv.string_strict)})

_UNSUPPORTED_RESPONSE_KEYS = (
    CONF_CAPTURE_RESPONSE,
    CONF_RESPONSE_TEMPLATE,
    CONF_ON_SUCCESS,
    CONF_ON_ERROR,
)


def _reject_response_keys(config):
    for key in _UNSUPPORTED_RESPONSE_KEYS:
        if key in config:
            raise cv.Invalid(
                f"`{key}` is not yet supported by the api_ble homeassistant.action "
                "(action response handling is a future phase)."
            )
    return config


HOMEASSISTANT_ACTION_ACTION_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(APIBLEServer),
            cv.Exclusive(CONF_SERVICE, group_of_exclusion=CONF_ACTION): cv.templatable(
                cv.string
            ),
            cv.Exclusive(CONF_ACTION, group_of_exclusion=CONF_ACTION): cv.templatable(
                cv.string
            ),
            cv.Optional(CONF_DATA, default={}): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_DATA_TEMPLATE, default={}): KEY_VALUE_SCHEMA,
            cv.Optional(CONF_VARIABLES, default={}): cv.Schema(
                {cv.string: cv.returning_lambda}
            ),
            # Accepted so we can raise a clear "not yet supported" error rather
            # than a generic "extra keys" one.
            cv.Optional(CONF_RESPONSE_TEMPLATE): cv.templatable(cv.string),
            cv.Optional(CONF_CAPTURE_RESPONSE): cv.boolean,
            cv.Optional(CONF_ON_SUCCESS): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
        }
    ),
    cv.has_exactly_one_key(CONF_SERVICE, CONF_ACTION),
    cv.rename_key(CONF_SERVICE, CONF_ACTION),
    _reject_response_keys,
)


@automation.register_action(
    "homeassistant.action",
    HomeAssistantServiceCallAction,
    HOMEASSISTANT_ACTION_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "homeassistant.service",
    HomeAssistantServiceCallAction,
    HOMEASSISTANT_ACTION_ACTION_SCHEMA,
    synchronous=True,
)
async def homeassistant_action_to_code(config, action_id, template_arg, args):
    serv = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, serv, False)
    templ = await cg.templatable(config[CONF_ACTION], args, cg.std_string)
    cg.add(var.set_service(templ))

    cg.add(var.init_data(len(config[CONF_DATA])))
    for key, value in config[CONF_DATA].items():
        templ = await cg.templatable(value, args, None)
        if isinstance(templ, str):
            templ = cg.FlashStringLiteral(templ)
        cg.add(var.add_data(cg.FlashStringLiteral(key), templ))

    cg.add(var.init_data_template(len(config[CONF_DATA_TEMPLATE])))
    for key, value in config[CONF_DATA_TEMPLATE].items():
        templ = await cg.templatable(value, args, None)
        if isinstance(templ, str):
            templ = cg.FlashStringLiteral(templ)
        cg.add(var.add_data_template(cg.FlashStringLiteral(key), templ))

    cg.add(var.init_variables(len(config[CONF_VARIABLES])))
    for key, value in config[CONF_VARIABLES].items():
        templ = await cg.templatable(value, args, None)
        cg.add(var.add_variable(cg.FlashStringLiteral(key), templ))

    return var


def _validate_homeassistant_event(value):
    value = cv.string(value)
    if not value.startswith("esphome."):
        raise cv.Invalid(
            "ESPHome can only generate Home Assistant events that begin with "
            "esphome. For example 'esphome.xyz'"
        )
    return value


HOMEASSISTANT_EVENT_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(APIBLEServer),
        cv.Required(CONF_EVENT): _validate_homeassistant_event,
        cv.Optional(CONF_DATA, default={}): KEY_VALUE_SCHEMA,
        cv.Optional(CONF_DATA_TEMPLATE, default={}): KEY_VALUE_SCHEMA,
        cv.Optional(CONF_VARIABLES, default={}): KEY_VALUE_SCHEMA,
    }
)


@automation.register_action(
    "homeassistant.event",
    HomeAssistantServiceCallAction,
    HOMEASSISTANT_EVENT_ACTION_SCHEMA,
    synchronous=True,
)
async def homeassistant_event_to_code(config, action_id, template_arg, args):
    serv = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, serv, True)
    templ = await cg.templatable(config[CONF_EVENT], args, cg.std_string)
    cg.add(var.set_service(templ))

    cg.add(var.init_data(len(config[CONF_DATA])))
    for key, value in config[CONF_DATA].items():
        templ = await cg.templatable(value, args, None)
        if isinstance(templ, str):
            templ = cg.FlashStringLiteral(templ)
        cg.add(var.add_data(cg.FlashStringLiteral(key), templ))

    cg.add(var.init_data_template(len(config[CONF_DATA_TEMPLATE])))
    for key, value in config[CONF_DATA_TEMPLATE].items():
        templ = await cg.templatable(value, args, None)
        if isinstance(templ, str):
            templ = cg.FlashStringLiteral(templ)
        cg.add(var.add_data_template(cg.FlashStringLiteral(key), templ))

    cg.add(var.init_variables(len(config[CONF_VARIABLES])))
    for key, value in config[CONF_VARIABLES].items():
        templ = await cg.templatable(value, args, None)
        cg.add(var.add_variable(cg.FlashStringLiteral(key), templ))

    return var


# ---------------------------------------------------------------------------
# Home Assistant entity state import (used by the api_ble sensor/text_sensor
# platforms). Mirrors homeassistant.HOME_ASSISTANT_IMPORT_SCHEMA but without the
# in-tree homeassistant component's `api` dependency.
# ---------------------------------------------------------------------------

HOME_ASSISTANT_IMPORT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.entity_id,
        cv.Optional(CONF_ATTRIBUTE): cv.string,
        cv.Optional(CONF_INTERNAL, default=True): cv.boolean,
    }
)


def setup_home_assistant_entity(var, config):
    cg.add(var.set_entity_id(config[CONF_ENTITY_ID]))
    if CONF_ATTRIBUTE in config:
        cg.add(var.set_attribute(config[CONF_ATTRIBUTE]))
