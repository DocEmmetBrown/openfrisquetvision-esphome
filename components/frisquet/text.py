import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

ZoneProgramText = frisquet_ns.class_("ZoneProgramText", text.Text, cg.Component)

CONFIG_SCHEMA = (
    text.text_schema(ZoneProgramText)
    .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text.new_text(config, min_length=84, max_length=84, pattern="^[0-9A-Fa-f]{84}$")
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_program_text(var))
