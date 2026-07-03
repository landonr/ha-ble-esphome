import esphome.codegen as cg
from esphome.components import text_sensor

from . import (
    HOME_ASSISTANT_IMPORT_SCHEMA,
    api_ble_ns,
    setup_home_assistant_entity,
)

DEPENDENCIES = ["api_ble"]

HomeassistantBLETextSensor = api_ble_ns.class_(
    "HomeassistantBLETextSensor", text_sensor.TextSensor, cg.Component
)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(HomeassistantBLETextSensor).extend(
    HOME_ASSISTANT_IMPORT_SCHEMA
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    setup_home_assistant_entity(var, config)
