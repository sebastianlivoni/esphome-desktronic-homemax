import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, cover, number, sensor, uart
from esphome.const import (
    CONF_ID,
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
CONF_POSITION1 = "position1"
CONF_POSITION2 = "position2"
CONF_POSITION1_HEIGHT = "position1_height"
CONF_POSITION2_HEIGHT = "position2_height"
CONF_REFRESH_POSITIONS = "refresh_positions"
CONF_SAVE_POSITION1 = "save_position1"
CONF_SAVE_POSITION2 = "save_position2"
CONF_SET_POSITION1 = "set_position1"
CONF_SET_POSITION2 = "set_position2"
CONF_MOVING = "moving"
CONF_STANDING = "standing"
CONF_STANDING_TIME = "standing_time"
CONF_COVER = "cover"

# Settings
CONF_MOVE_DURATION = "move_duration"
CONF_STOP_EARLY = "stop_early"
CONF_MIN_HEIGHT = "min_height"
CONF_MAX_HEIGHT = "max_height"
CONF_STANDING_HEIGHT = "standing_height"
CONF_POSITION1_COMMAND = "position1_command"
CONF_POSITION2_COMMAND = "position2_command"
CONF_SAVE_POSITION1_COMMAND = "save_position1_command"
CONF_SAVE_POSITION2_COMMAND = "save_position2_command"
CONF_POSITION1_REPORT = "position1_report"
CONF_POSITION2_REPORT = "position2_report"

homemax_ns = cg.esphome_ns.namespace("homemax_desk")
HomeMaxDesk = homemax_ns.class_("HomeMaxDesk", cg.Component, uart.UARTDevice)
DeskButton = homemax_ns.class_("DeskButton", button.Button)
DeskHeightNumber = homemax_ns.class_("DeskHeightNumber", number.Number)
DeskPositionNumber = homemax_ns.class_("DeskPositionNumber", number.Number)
DeskCover = homemax_ns.class_("DeskCover", cover.Cover)

# Must match the ButtonAction values in homemax_desk.h
BUTTON_ACTIONS = {
    CONF_MOVE_UP: 0,
    CONF_MOVE_DOWN: 1,
    CONF_STOP: 2,
    CONF_POSITION1: 3,
    CONF_POSITION2: 4,
    CONF_REFRESH_POSITIONS: 5,
    CONF_SAVE_POSITION1: 6,
    CONF_SAVE_POSITION2: 7,
}

POSITION_NUMBERS = {
    CONF_SET_POSITION1: 1,
    CONF_SET_POSITION2: 2,
}


def _height_sensor(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CENTIMETER,
        accuracy_decimals=1,
        icon=icon,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _height_number(class_, icon):
    return number.number_schema(class_, unit_of_measurement=UNIT_CENTIMETER, icon=icon)


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
            cv.Optional(CONF_TARGET_HEIGHT): _height_number(
                DeskHeightNumber, "mdi:human-male-height-variant"
            ),
            cv.Optional(CONF_COVER): cover.cover_schema(
                DeskCover, icon="mdi:desk"
            ),
            # Memory positions
            cv.Optional(CONF_POSITION1): button.button_schema(
                DeskButton, icon="mdi:numeric-1-box"
            ),
            cv.Optional(CONF_POSITION2): button.button_schema(
                DeskButton, icon="mdi:numeric-2-box"
            ),
            cv.Optional(CONF_POSITION1_HEIGHT): _height_sensor(
                "mdi:numeric-1-box-outline"
            ),
            cv.Optional(CONF_POSITION2_HEIGHT): _height_sensor(
                "mdi:numeric-2-box-outline"
            ),
            cv.Optional(CONF_SAVE_POSITION1): button.button_schema(
                DeskButton, icon="mdi:content-save"
            ),
            cv.Optional(CONF_SAVE_POSITION2): button.button_schema(
                DeskButton, icon="mdi:content-save"
            ),
            cv.Optional(CONF_SET_POSITION1): _height_number(
                DeskPositionNumber, "mdi:numeric-1-box-multiple-outline"
            ),
            cv.Optional(CONF_SET_POSITION2): _height_number(
                DeskPositionNumber, "mdi:numeric-2-box-multiple-outline"
            ),
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
            # Settings
            cv.Optional(
                CONF_MOVE_DURATION, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STOP_EARLY, default=5): cv.int_range(min=0, max=50),
            cv.Optional(CONF_MIN_HEIGHT, default=50.0): cv.float_range(min=0, max=300),
            cv.Optional(CONF_MAX_HEIGHT, default=140.0): cv.float_range(min=0, max=300),
            cv.Optional(CONF_STANDING_HEIGHT, default=95.0): cv.float_range(
                min=0, max=300
            ),
            cv.Optional(CONF_POSITION1_COMMAND, default=0x05): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION2_COMMAND, default=0x06): cv.hex_uint8_t,
            cv.Optional(CONF_SAVE_POSITION1_COMMAND, default=0x03): cv.hex_uint8_t,
            cv.Optional(CONF_SAVE_POSITION2_COMMAND, default=0x04): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION1_REPORT, default=0x25): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION2_REPORT, default=0x26): cv.hex_uint8_t,
        }
    )
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
    cg.add(var.set_move_duration(config[CONF_MOVE_DURATION]))
    cg.add(var.set_stop_early(config[CONF_STOP_EARLY]))
    cg.add(var.set_height_limits(config[CONF_MIN_HEIGHT], config[CONF_MAX_HEIGHT]))
    cg.add(var.set_standing_height(config[CONF_STANDING_HEIGHT]))
    cg.add(
        var.set_position_commands(
            config[CONF_POSITION1_COMMAND], config[CONF_POSITION2_COMMAND]
        )
    )
    cg.add(
        var.set_save_commands(
            config[CONF_SAVE_POSITION1_COMMAND], config[CONF_SAVE_POSITION2_COMMAND]
        )
    )
    cg.add(
        var.set_position_reports(
            config[CONF_POSITION1_REPORT], config[CONF_POSITION2_REPORT]
        )
    )

    # Sensors
    if CONF_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_HEIGHT])
        cg.add(var.set_height_sensor(sens))
    if CONF_POSITION1_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_POSITION1_HEIGHT])
        cg.add(var.set_position1_sensor(sens))
    if CONF_POSITION2_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_POSITION2_HEIGHT])
        cg.add(var.set_position2_sensor(sens))
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

    # Buttons
    for key, action in BUTTON_ACTIONS.items():
        if key in config:
            btn = await button.new_button(config[key])
            cg.add(btn.set_parent(var))
            cg.add(btn.set_action(action))

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

    for key, slot in POSITION_NUMBERS.items():
        if key in config:
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
