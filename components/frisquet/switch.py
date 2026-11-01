import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_TYPE

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

KillSwitch = frisquet_ns.class_("KillSwitch", switch.Switch, cg.Component)
ZoneBoostSwitch = frisquet_ns.class_("ZoneBoostSwitch", switch.Switch, cg.Component)

# kill (default): ON means "emergency stop engaged"; the default restore mode keeps it engaged after a reboot.
# zone1_boost: comfort +1.0 C for at most 6 h (Confort, or an Auto comfort period); its state follows the zone block.
CONFIG_SCHEMA = cv.typed_schema(
    {
        "kill": switch.switch_schema(KillSwitch, default_restore_mode="RESTORE_DEFAULT_ON")
        .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
        .extend(cv.COMPONENT_SCHEMA),
        "zone1_boost": switch.switch_schema(ZoneBoostSwitch, default_restore_mode="DISABLED")
        .extend({cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio)})
        .extend(cv.COMPONENT_SCHEMA),
    },
    default_type="kill",
    lower=True,
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    if config[CONF_TYPE] == "zone1_boost":
        cg.add(parent.set_boost_switch(var))
