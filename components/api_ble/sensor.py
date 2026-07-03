import esphome.codegen as cg
from esphome.components import sensor

from . import (
    HOME_ASSISTANT_IMPORT_SCHEMA,
    api_ble_ns,
    setup_home_assistant_entity,
)

DEPENDENCIES = ["api_ble"]

HomeassistantBLESensor = api_ble_ns.class_(
    "HomeassistantBLESensor", sensor.Sensor, cg.Component
)

CONFIG_SCHEMA = sensor.sensor_schema(HomeassistantBLESensor, accuracy_decimals=1).extend(
    HOME_ASSISTANT_IMPORT_SCHEMA
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    setup_home_assistant_entity(var, config)
