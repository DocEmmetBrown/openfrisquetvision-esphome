import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_TYPE, UNIT_CELSIUS

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

# Field index = position in the zone block (comfort, reduced, frost); frost may not exceed 15 degrees.
FIELDS = {
    "zone1_comfort": (0, 5.0, 30.0),
    "zone1_reduced": (1, 5.0, 30.0),
    "zone1_frost": (2, 5.0, 15.0),
}

ZoneSetpointNumber = frisquet_ns.class_("ZoneSetpointNumber", number.Number, cg.Component)

CONFIG_SCHEMA = (
    number.number_schema(ZoneSetpointNumber, unit_of_measurement=UNIT_CELSIUS)
    .extend(
        {
            cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio),
            cv.Required(CONF_TYPE): cv.one_of(*FIELDS, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    field, minimum, maximum = FIELDS[config[CONF_TYPE]]
    var = await number.new_number(config, min_value=minimum, max_value=maximum, step=0.5)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_field(field))
    cg.add(parent.set_zone_number(field, var))
