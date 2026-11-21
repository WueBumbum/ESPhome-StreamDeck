import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32"]

streamdeck_ns = cg.esphome_ns.namespace("streamdeck_poc")
StreamDeckPoC = streamdeck_ns.class_("StreamDeckPoC", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(StreamDeckPoC)}).extend(
    cv.COMPONENT_SCHEMA
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_build_flag("-DSTREAMDECK_POC=1")
