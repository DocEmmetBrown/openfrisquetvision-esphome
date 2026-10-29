import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text
from esphome.const import CONF_TYPE

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

ZoneProgramText = frisquet_ns.class_("ZoneProgramText", text.Text, cg.Component)
ExploreText = frisquet_ns.class_("ExploreText", text.Text, cg.Component)

# program: zone 1 weekly programme (84 hexadecimal characters).
# explore: "ADDR WORDS" in hexadecimal, one exploratory read (needs `explore: true` on the frisquet component).
TYPES = {"program": ZoneProgramText, "explore": ExploreText}

CONFIG_SCHEMA = cv.typed_schema(
    {
        "program": text.text_schema(ZoneProgramText)
        .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
        .extend(cv.COMPONENT_SCHEMA),
        "explore": text.text_schema(ExploreText)
        .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
        .extend(cv.COMPONENT_SCHEMA),
    },
    default_type="program",
)


async def to_code(config):
    if config[CONF_TYPE] == "explore":
        var = await text.new_text(config, min_length=3, max_length=9, pattern="^[0-9A-Fa-f]{1,4} [0-9A-Fa-f]{1,2}$")
    else:
        var = await text.new_text(config, min_length=84, max_length=84, pattern="^[0-9A-Fa-f]{84}$")
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_TYPE] == "program":
        cg.add(parent.set_program_text(var))
