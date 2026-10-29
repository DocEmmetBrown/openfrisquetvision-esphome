import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_TYPE

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

# Same labels as the MQTT firmware. Order and values must match HOT_WATER_MODES / ZONE_MODES in frisquet_radio.cpp.
TYPES = {
    "hot_water": ("kHotWater", "set_hot_water_select", ["Max", "Eco", "Eco Horaires", "Eco+", "Eco+ Horaires", "Stop"]),
    "zone1_mode": (
        "kZone1Mode",
        "set_zone_mode_select",
        ["Auto", "Dérogation Confort", "Dérogation Réduit", "Confort", "Réduit", "Hors Gel"],
    ),
}

FrisquetSelect = frisquet_ns.class_("FrisquetSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(FrisquetSelect)
    .extend(
        {
            cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio),
            cv.Optional(CONF_TYPE, default="hot_water"): cv.one_of(*TYPES, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    kind, setter, options = TYPES[config[CONF_TYPE]]
    var = await select.new_select(config, options=options)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_kind(getattr(FrisquetSelect.enum("Kind"), kind)))
    cg.add(getattr(parent, setter)(var))
