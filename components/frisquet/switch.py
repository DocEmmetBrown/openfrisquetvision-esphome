import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

KillSwitch = frisquet_ns.class_("KillSwitch", switch.Switch, cg.Component)

# ON means "emergency stop engaged". The default restore mode keeps it engaged after a reboot.
CONFIG_SCHEMA = (
    switch.switch_schema(KillSwitch, default_restore_mode="RESTORE_DEFAULT_ON")
    .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
