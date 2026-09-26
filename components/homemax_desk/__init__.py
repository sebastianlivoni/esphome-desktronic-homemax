import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, cover, number, sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_PERCENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_MOVING,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CENTIMETER,
    UNIT_MINUTE,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "button", "number", "cover"]

# Entities
CONF_HEIGHT = "height"
CONF_MOVE_UP = "move_up"
CONF_MOVE_DOWN = "move_down"
CONF_STOP = "stop"
CONF_TARGET_HEIGHT = "target_height"
CONF_REFRESH_POSITIONS = "refresh_positions"
CONF_MOVING = "moving"
CONF_STANDING = "standing"
CONF_STANDING_TIME = "standing_time"
CONF_COVER = "cover"
CONF_HEIGHT_PERCENT = "height_percent"
CONF_HEIGHT_MIN = "height_min"
CONF_HEIGHT_MAX = "height_max"
CONF_USER_HEIGHT_MIN = "user_height_min"
CONF_USER_HEIGHT_MAX = "user_height_max"
CONF_CONTROLLER_CONNECTED = "controller_connected"

# Settings
CONF_MIN_HEIGHT = "min_height"
CONF_MAX_HEIGHT = "max_height"
CONF_STANDING_HEIGHT = "standing_height"
CONF_AUTO_LIMITS = "auto_limits"
CONF_POLL_INTERVAL = "poll_interval"

# Memory positions 1-4: entity keys and default command / report bytes
POSITIONS = (1, 2, 3, 4)
DEFAULT_POSITION_COMMANDS = {1: 0x05, 2: 0x06, 3: 0x27, 4: 0x28}
DEFAULT_SAVE_COMMANDS = {1: 0x03, 2: 0x04, 3: 0x25, 4: 0x26}
DEFAULT_POSITION_REPORTS = {1: 0x25, 2: 0x26, 3: 0x27, 4: 0x28}

homemax_ns = cg.esphome_ns.namespace("homemax_desk")
HomeMaxDesk = homemax_ns.class_("HomeMaxDesk", cg.Component, uart.UARTDevice)
DeskButton = homemax_ns.class_("DeskButton", button.Button)
DeskHeightNumber = homemax_ns.class_("DeskHeightNumber", number.Number)
DeskPositionNumber = homemax_ns.class_("DeskPositionNumber", number.Number)
DeskCover = homemax_ns.class_("DeskCover", cover.Cover)

# Must match the ButtonAction values in homemax_desk.h
ACTION_MOVE_UP = 0
ACTION_MOVE_DOWN = 1
ACTION_STOP = 2
ACTION_GOTO_POSITION = 3
ACTION_SAVE_POSITION = 4
ACTION_REFRESH_POSITIONS = 5

# key -> (action, slot)
BUTTON_ACTIONS = {
    CONF_MOVE_UP: (ACTION_MOVE_UP, 0),
    CONF_MOVE_DOWN: (ACTION_MOVE_DOWN, 0),
    CONF_STOP: (ACTION_STOP, 0),
    CONF_REFRESH_POSITIONS: (ACTION_REFRESH_POSITIONS, 0),
}
for _n in POSITIONS:
    BUTTON_ACTIONS[f"position{_n}"] = (ACTION_GOTO_POSITION, _n)
    BUTTON_ACTIONS[f"save_position{_n}"] = (ACTION_SAVE_POSITION, _n)


def _height_sensor(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CENTIMETER,
        accuracy_decimals=1,
        icon=icon,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _height_number(class_, icon):
    return number.number_schema(class_, unit_of_measurement=UNIT_CENTIMETER, icon=icon)


def _position_schema():
    schema = {}
    for n in POSITIONS:
        schema[cv.Optional(f"position{n}")] = button.button_schema(
            DeskButton, icon=f"mdi:numeric-{n}-box"
        )
        schema[cv.Optional(f"position{n}_height")] = _height_sensor(
            f"mdi:numeric-{n}-box-outline"
        )
        schema[cv.Optional(f"save_position{n}")] = button.button_schema(
            DeskButton, icon="mdi:content-save"
        )
        schema[cv.Optional(f"set_position{n}")] = _height_number(
            DeskPositionNumber, f"mdi:numeric-{n}-box-multiple-outline"
        )
        schema[
            cv.Optional(f"position{n}_command", default=DEFAULT_POSITION_COMMANDS[n])
        ] = cv.hex_uint8_t
        schema[
            cv.Optional(f"save_position{n}_command", default=DEFAULT_SAVE_COMMANDS[n])
        ] = cv.hex_uint8_t
        schema[
            cv.Optional(f"position{n}_report", default=DEFAULT_POSITION_REPORTS[n])
        ] = cv.hex_uint8_t
    return schema


def _validate_heights(config):
    if config[CONF_MIN_HEIGHT] >= config[CONF_MAX_HEIGHT]:
        raise cv.Invalid("min_height must be lower than max_height")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeMaxDesk),
            # Height and movement
            cv.Optional(CONF_HEIGHT): _height_sensor("mdi:arrow-expand-vertical"),
            cv.Optional(CONF_MOVE_UP): button.button_schema(
                DeskButton, icon="mdi:arrow-up-bold"
            ),
            cv.Optional(CONF_MOVE_DOWN): button.button_schema(
                DeskButton, icon="mdi:arrow-down-bold"
            ),
            cv.Optional(CONF_STOP): button.button_schema(DeskButton, icon="mdi:stop"),
            cv.Optional(CONF_HEIGHT_PERCENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                icon="mdi:percent",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_HEIGHT_MIN): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:arrow-collapse-down",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_HEIGHT_MAX): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:arrow-collapse-up",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_USER_HEIGHT_MIN): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:arrow-collapse-down",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_USER_HEIGHT_MAX): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:arrow-collapse-up",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_TARGET_HEIGHT): _height_number(
                DeskHeightNumber, "mdi:human-male-height-variant"
            ),
            cv.Optional(CONF_COVER): cover.cover_schema(
                DeskCover, icon="mdi:desk"
            ),
            # Memory positions (positions 1-4 are added below)
            cv.Optional(CONF_REFRESH_POSITIONS): button.button_schema(
                DeskButton, icon="mdi:refresh"
            ),
            # Status
            cv.Optional(CONF_MOVING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_MOVING
            ),
            cv.Optional(CONF_STANDING): binary_sensor.binary_sensor_schema(
                icon="mdi:human-handsup"
            ),
            cv.Optional(CONF_STANDING_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_MINUTE,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:timer-outline",
            ),
            cv.Optional(CONF_CONTROLLER_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Settings
            cv.Optional(CONF_AUTO_LIMITS, default=True): cv.boolean,
            cv.Optional(
                CONF_POLL_INTERVAL, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MIN_HEIGHT, default=50.0): cv.float_range(min=0, max=300),
            cv.Optional(CONF_MAX_HEIGHT, default=140.0): cv.float_range(min=0, max=300),
            cv.Optional(CONF_STANDING_HEIGHT, default=95.0): cv.float_range(
                min=0, max=300
            ),
        }
    )
    .extend(_position_schema())
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate_heights,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "homemax_desk", baud_rate=9600, require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    # Settings
    cg.add(var.set_height_limits(config[CONF_MIN_HEIGHT], config[CONF_MAX_HEIGHT]))
    cg.add(var.set_standing_height(config[CONF_STANDING_HEIGHT]))
    cg.add(var.set_auto_limits(config[CONF_AUTO_LIMITS]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    for n in POSITIONS:
        cg.add(var.set_position_command(n, config[f"position{n}_command"]))
        cg.add(var.set_save_command(n, config[f"save_position{n}_command"]))
        cg.add(var.set_position_report(n, config[f"position{n}_report"]))

    # Sensors
    if CONF_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_HEIGHT])
        cg.add(var.set_height_sensor(sens))
    for n in POSITIONS:
        if (key := f"position{n}_height") in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(var.set_position_sensor(n, sens))
    for key, setter in (
        (CONF_HEIGHT_PERCENT, "set_height_percent_sensor"),
        (CONF_HEIGHT_MIN, "set_height_min_sensor"),
        (CONF_HEIGHT_MAX, "set_height_max_sensor"),
        (CONF_USER_HEIGHT_MIN, "set_user_min_sensor"),
        (CONF_USER_HEIGHT_MAX, "set_user_max_sensor"),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
    if CONF_STANDING_TIME in config:
        sens = await sensor.new_sensor(config[CONF_STANDING_TIME])
        cg.add(var.set_standing_time_sensor(sens))

    # Binary sensors
    if CONF_MOVING in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_MOVING])
        cg.add(var.set_moving_sensor(bs))
    if CONF_STANDING in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_STANDING])
        cg.add(var.set_standing_sensor(bs))
    if CONF_CONTROLLER_CONNECTED in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_CONTROLLER_CONNECTED])
        cg.add(var.set_connected_sensor(bs))

    # Buttons
    for key, (action, slot) in BUTTON_ACTIONS.items():
        if key in config:
            btn = await button.new_button(config[key])
            cg.add(btn.set_parent(var))
            cg.add(btn.set_action(action))
            cg.add(btn.set_slot(slot))

    # Numbers
    if CONF_TARGET_HEIGHT in config:
        num = await number.new_number(
            config[CONF_TARGET_HEIGHT],
            min_value=config[CONF_MIN_HEIGHT],
            max_value=config[CONF_MAX_HEIGHT],
            step=0.5,
        )
        cg.add(num.set_parent(var))
        cg.add(var.set_target_number(num))

    for slot in POSITIONS:
        if (key := f"set_position{slot}") in config:
            num = await number.new_number(
                config[key],
                min_value=config[CONF_MIN_HEIGHT],
                max_value=config[CONF_MAX_HEIGHT],
                step=0.5,
            )
            cg.add(num.set_parent(var))
            cg.add(num.set_slot(slot))
            cg.add(var.set_position_number(slot, num))

    # Cover
    if CONF_COVER in config:
        cov = await cover.new_cover(config[CONF_COVER])
        cg.add(cov.set_parent(var))
        cg.add(var.set_cover(cov))
