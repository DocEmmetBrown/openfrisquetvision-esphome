import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import FrisquetRadio, frisquet_ns

DEPENDENCIES = ["frisquet"]

CONF_FRISQUET_ID = "frisquet_id"

# recover_network: listen 30 s on the broadcast network for the boiler's pairing offer, keep its NetworkID.
# associate: same, then confirm as the Connect (needs `pairing: true`); the association byte changes too.
# forget_network: back to the YAML NetworkID and association.
TYPES = {"recover_network": "kRecoverNetwork", "associate": "kAssociate", "forget_network": "kForgetNetwork"}

FrisquetButton = frisquet_ns.class_("FrisquetButton", button.Button, cg.Component)

CONFIG_SCHEMA = (
    button.button_schema(FrisquetButton, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(
        {
            cv.GenerateID(CONF_FRISQUET_ID): cv.use_id(FrisquetRadio),
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_FRISQUET_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_kind(getattr(FrisquetButton.enum("Kind"), TYPES[config[CONF_TYPE]])))
