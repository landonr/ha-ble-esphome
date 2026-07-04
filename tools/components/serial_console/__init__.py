"""Dev tool: line-based commands over the logger's USB serial port.

The ESP-IDF logger installs the UART driver with an RX buffer it never
reads; this component polls that RX side and fires on_command with each
received line. Lets a host script drive the device (e.g. trigger
homeassistant.action sends over BLE) without touching the buttons.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID, Framework

CODEOWNERS = ["@landorg"]
DEPENDENCIES = ["logger"]

CONF_ON_COMMAND = "on_command"

serial_console_ns = cg.esphome_ns.namespace("serial_console")
SerialConsole = serial_console_ns.class_("SerialConsole", cg.Component)
CommandTrigger = serial_console_ns.class_(
    "CommandTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SerialConsole),
            cv.Optional(CONF_ON_COMMAND): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CommandTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework(Framework.ESP_IDF),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for conf in config.get(CONF_ON_COMMAND, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "x")], conf
        )
