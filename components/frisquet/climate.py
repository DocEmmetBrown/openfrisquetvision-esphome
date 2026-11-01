import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

FrisquetClimate = frisquet_ns.class_("FrisquetClimate", climate.Climate, cg.Component)

# Zone 1 as one thermostat: AUTO = programme, HEAT = permanent (preset Confort / Réduit), OFF = Hors gel (frost
# protection stays on), preset BOOST = comfort +1.0 C. The target is the setpoint of the active mode.
CONFIG_SCHEMA = (
    climate.climate_schema(FrisquetClimate)
    .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_zone_climate(var))
