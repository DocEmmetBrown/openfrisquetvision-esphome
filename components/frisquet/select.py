import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

# Same labels as the MQTT firmware. Order and values must match HOT_WATER_MODES in frisquet_radio.cpp.
HOT_WATER_OPTIONS = ["Max", "Eco", "Eco Horaires", "Eco+", "Eco+ Horaires", "Stop"]

HotWaterSelect = frisquet_ns.class_("HotWaterSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(HotWaterSelect)
    .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await select.new_select(config, options=HOT_WATER_OPTIONS)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_hot_water_select(var))
