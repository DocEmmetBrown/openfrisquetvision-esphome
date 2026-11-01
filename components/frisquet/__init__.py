"""Frisquet boiler radio (receive only: no transmit path exists in this build)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_PRESSURE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_SECOND,
)

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor"]
CODEOWNERS = []

frisquet_ns = cg.esphome_ns.namespace("frisquet_radio")
FrisquetRadio = frisquet_ns.class_("FrisquetRadio", cg.Component)

CONF_TX = "tx"
CONF_DRY_RUN = "dry_run"
CONF_ASSOCIATION = "association"
CONF_NETWORK_ID = "network_id"
CONF_BOARD = "board"
CONF_RX_BANDWIDTH = "rx_bandwidth"
CONF_RX_BOOSTED_GAIN = "rx_boosted_gain"
CONF_LNA_BYPASS = "lna_bypass"
CONF_PAIRING = "pairing"
CONF_EXPLORE = "explore"
CONF_SATELLITE_OVERRIDE = "satellite_override"
CONF_SATELLITE_ASSOCIATION = "satellite_association"
CONF_BURNER = "burner"
DEVICE_CLASS_HEAT = "heat"

BOARDS = {
    # Heltec WiFi LoRa 32 V4.3: SX1262 + KCT8103L front-end module.
    "heltec_v4_3": dict(cs=8, irq=14, reset=12, busy=13, sck=9, miso=11, mosi=10, fem=True),
}

# SX1262 GFSK receive bandwidths accepted by RadioLib (kHz).
RX_BANDWIDTHS = [
    4.8, 5.8, 7.3, 9.7, 11.7, 14.6, 19.5, 23.4, 29.3, 39.0, 46.9, 58.6, 78.2, 93.8,
    117.3, 156.2, 187.2, 234.3, 312.0, 373.6, 467.0,
]


def _temperature():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _counter():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _energy_total():
    return sensor.sensor_schema(
        unit_of_measurement="kWh",
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    )


# The order below defines the C++ SensorId enum (frisquet_radio.h): keep both in sync.
SENSORS = {
    "zone1_ambient_temperature": _temperature(),
    "zone1_setpoint": _temperature(),
    "rssi": sensor.sensor_schema(
        unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "frames_valid": _counter(),
    "frames_repaired": _counter(),
    "frames_rejected": _counter(),
    "seconds_since_frame": sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    # Filled only when a Connect is on the air and its read answers are overheard.
    "hot_water_temperature": _temperature(),
    "heating_body_temperature": _temperature(),
    "outdoor_temperature": _temperature(),
    "flue_temperature": _temperature(),
    "zone1_flow_temperature": _temperature(),
    "zone1_flow_setpoint": _temperature(),
    "pressure": sensor.sensor_schema(
        unit_of_measurement="bar",
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_PRESSURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "consumption_hot_water": sensor.sensor_schema(
        unit_of_measurement="kWh", accuracy_decimals=0, device_class=DEVICE_CLASS_ENERGY
    ),
    "consumption_heating": sensor.sensor_schema(
        unit_of_measurement="kWh", accuracy_decimals=0, device_class=DEVICE_CLASS_ENERGY
    ),
    "annual_consumption_hot_water": _energy_total(),
    "annual_consumption_heating": _energy_total(),
    # Transmit-side counters (stay at 0 when tx: none).
    "acks_sent": _counter(),
    "reads_ok": _counter(),
    "reads_failed": _counter(),
    "tx_dry_runs": _counter(),
    "tx_blocked": _counter(),
    # Live heating-body temperature (0xA05E, read every 15 s outside the satellites' window).
    "heating_body_live": _temperature(),
}

# Same rule for the C++ TextId enum.
TEXT_SENSORS = {
    "zone1_mode": text_sensor.text_sensor_schema(),
    "boiler_state": text_sensor.text_sensor_schema(),
    "boiler_clock": text_sensor.text_sensor_schema(),
    "boiler_activity": text_sensor.text_sensor_schema(),
    "hot_water_mode": text_sensor.text_sensor_schema(),
    # Raw bytes for diagnosis: broadcast state/mode, and the last relayed zone block.
    "zone1_raw": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    "zone_block": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # State of the last hot-water mode write: queued, sending, acked, verified, failed, not-sent.
    "hot_water_write": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # Same states for the last zone 1 setpoint write.
    "zone_write": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # Zone 1 programme as readable ranges ("Dim 09:30-22:00 | Lun 08:00-21:30 | ...").
    "zone_program": text_sensor.text_sensor_schema(),
    # Zone 1 block: inconnu / restauré (non confirmé) / confirmé. Writes need "confirmé".
    "zone_block_state": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # Network in use: "05:D9:8D:C9 (YAML), association E3" or "(appris)" once pairing learned it.
    "network": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    # Last pairing session: listening, confirming, succeeded, failed, cancelled.
    "pairing_state": text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
}


def network_id(value):
    value = cv.string_strict(value)
    parts = value.split(":")
    if len(parts) != 4:
        raise cv.Invalid("network_id must look like AA:BB:CC:DD")
    try:
        octets = [int(part, 16) for part in parts]
    except ValueError as err:
        raise cv.Invalid("network_id must be hexadecimal") from err
    if any(not 0 <= octet <= 255 for octet in octets):
        raise cv.Invalid("network_id bytes must be 00..FF")
    if octets == [0xFF] * 4:
        raise cv.Invalid("FF:FF:FF:FF is the broadcast network used for pairing")
    return octets


def _validate_tx(config):
    if config[CONF_TX] != "none":
        if CONF_ASSOCIATION not in config:
            raise cv.Invalid(
                "association (the Connect's association byte, e.g. 0xE3) is required when tx is not 'none'"
            )
        if config[CONF_ASSOCIATION] == 0xFF:
            raise cv.Invalid("association 0xFF means 'not associated'")
        if config[CONF_SATELLITE_OVERRIDE] and CONF_SATELLITE_ASSOCIATION not in config:
            raise cv.Invalid("satellite_override needs satellite_association (the satellite's association byte, e.g. 0xFA)")
    elif config[CONF_PAIRING] or config[CONF_EXPLORE]:
        raise cv.Invalid("pairing and explore need a transmit path (tx: read, ack or write)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FrisquetRadio),
        cv.Required(CONF_BOARD): cv.one_of(*BOARDS, lower=True),
        cv.Required(CONF_NETWORK_ID): network_id,
        # none: no transmit path is compiled. read: scheduled reads only. ack: reads and the
        # acknowledgement of the boiler's zone relays (what a Connect must do).
        cv.Optional(CONF_TX, default="none"): cv.one_of("none", "read", "ack", "write", lower=True),
        # While true, frames are built, checked and logged but never transmitted.
        cv.Optional(CONF_DRY_RUN, default=True): cv.boolean,
        cv.Optional(CONF_ASSOCIATION): cv.hex_uint8_t,
        cv.Optional(CONF_RX_BANDWIDTH, default=234.3): cv.one_of(*RX_BANDWIDTHS, float=True),
        # SX1262 boosted receive gain (about +3 dB, a little more current): for the satellites' weak frames.
        cv.Optional(CONF_RX_BOOSTED_GAIN, default=False): cv.boolean,
        cv.Optional(CONF_LNA_BYPASS, default=False): cv.boolean,
        # Allows the association button to transmit its confirmation (it changes the NetworkID and the
        # association byte). NetworkID recovery only listens and is always available.
        cv.Optional(CONF_PAIRING, default=False): cv.boolean,
        # Allows exploratory reads of any address (text entity of type `explore`), to map the boiler's memory.
        cv.Optional(CONF_EXPLORE, default=False): cv.boolean,
        # Allows consigne exchanges in the name of the zone 1 satellite (its association byte is required), for direct
        # setpoints. Experimental.
        cv.Optional(CONF_SATELLITE_OVERRIDE, default=False): cv.boolean,
        cv.Optional(CONF_SATELLITE_ASSOCIATION): cv.hex_uint8_t,
        # Burner on: the live heating-body temperature rises (0xA05E every 15 s; ~30-40 s of thermal lag).
        cv.Optional(CONF_BURNER): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_HEAT),
        **{cv.Optional(key): schema for key, schema in SENSORS.items()},
        **{cv.Optional(key): schema for key, schema in TEXT_SENSORS.items()},
    }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_tx,
)




async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_library("SPI", None)
    cg.add_library("jgromes/RadioLib", "7.8.1")
    cg.add_define("FRISQUET_SENSOR_COUNT", len(SENSORS))
    cg.add_define("FRISQUET_TEXT_COUNT", len(TEXT_SENSORS))

    board = BOARDS[config[CONF_BOARD]]
    cg.add(
        var.set_pins(
            board["cs"], board["irq"], board["reset"], board["busy"],
            board["sck"], board["miso"], board["mosi"],
        )
    )
    cg.add(var.set_fem_heltec_v4_3(board["fem"]))
    octets = config[CONF_NETWORK_ID]
    cg.add(var.set_network_id(octets[0], octets[1], octets[2], octets[3]))
    cg.add(var.set_rx_bandwidth(config[CONF_RX_BANDWIDTH]))
    cg.add(var.set_rx_boosted_gain(config[CONF_RX_BOOSTED_GAIN]))
    cg.add(var.set_lna_bypass(config[CONF_LNA_BYPASS]))

    if config[CONF_TX] != "none":
        # The transmit path only exists in builds that ask for it.
        cg.add_define("FRISQUET_TX_ENABLED")
        level = {"read": 1, "ack": 2, "write": 3}[config[CONF_TX]]
        cg.add(var.set_tx(level, config[CONF_DRY_RUN], config[CONF_ASSOCIATION]))
        cg.add(var.set_pairing_allowed(config[CONF_PAIRING]))
        cg.add(var.set_explore_allowed(config[CONF_EXPLORE]))
        if config[CONF_SATELLITE_OVERRIDE]:
            cg.add(var.set_satellite_override(config[CONF_SATELLITE_ASSOCIATION]))
        cg.add(var.set_live_body_reads(CONF_BURNER in config or "heating_body_live" in config))

    for index, key in enumerate(SENSORS):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(var.set_sensor(index, sens))
    for index, key in enumerate(TEXT_SENSORS):
        if key in config:
            sens = await text_sensor.new_text_sensor(config[key])
            cg.add(var.set_text_sensor(index, sens))
    if CONF_BURNER in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_BURNER])
        cg.add(var.set_burner_sensor(sens))
