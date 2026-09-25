import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, number, sensor, uart
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
    UNIT_CENTIMETER,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "button", "number"]

CONF_HEIGHT = "height"
CONF_MOVE_UP = "move_up"
CONF_MOVE_DOWN = "move_down"
CONF_STOP = "stop"
CONF_POSITION1 = "position1"
CONF_POSITION2 = "position2"
CONF_POSITION1_COMMAND = "position1_command"
CONF_POSITION2_COMMAND = "position2_command"
CONF_POSITION1_HEIGHT = "position1_height"
CONF_POSITION2_HEIGHT = "position2_height"
CONF_POSITION1_REPORT = "position1_report"
CONF_POSITION2_REPORT = "position2_report"
CONF_REFRESH_POSITIONS = "refresh_positions"
CONF_SAVE_POSITION1 = "save_position1"
CONF_SAVE_POSITION2 = "save_position2"
CONF_SAVE_POSITION1_COMMAND = "save_position1_command"
CONF_SAVE_POSITION2_COMMAND = "save_position2_command"
CONF_SET_POSITION1 = "set_position1"
CONF_SET_POSITION2 = "set_position2"
CONF_TARGET_HEIGHT = "target_height"
CONF_MOVE_DURATION = "move_duration"
CONF_STOP_EARLY = "stop_early"
CONF_MIN_HEIGHT = "min_height"
CONF_MAX_HEIGHT = "max_height"

homemax_ns = cg.esphome_ns.namespace("homemax_desk")
HomeMaxDesk = homemax_ns.class_("HomeMaxDesk", cg.Component, uart.UARTDevice)
DeskButton = homemax_ns.class_("DeskButton", button.Button)
DeskHeightNumber = homemax_ns.class_("DeskHeightNumber", number.Number)
DeskPositionNumber = homemax_ns.class_("DeskPositionNumber", number.Number)

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

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeMaxDesk),
            cv.Optional(CONF_HEIGHT): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:arrow-expand-vertical",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_MOVE_UP): button.button_schema(
                DeskButton, icon="mdi:arrow-up-bold"
            ),
            cv.Optional(CONF_MOVE_DOWN): button.button_schema(
                DeskButton, icon="mdi:arrow-down-bold"
            ),
            cv.Optional(CONF_STOP): button.button_schema(
                DeskButton, icon="mdi:stop"
            ),
            cv.Optional(CONF_POSITION1): button.button_schema(
                DeskButton, icon="mdi:numeric-1-box"
            ),
            cv.Optional(CONF_POSITION2): button.button_schema(
                DeskButton, icon="mdi:numeric-2-box"
            ),
            cv.Optional(CONF_SAVE_POSITION1): button.button_schema(
                DeskButton, icon="mdi:content-save"
            ),
            cv.Optional(CONF_SAVE_POSITION2): button.button_schema(
                DeskButton, icon="mdi:content-save"
            ),
            cv.Optional(CONF_SET_POSITION1): number.number_schema(
                DeskPositionNumber,
                unit_of_measurement=UNIT_CENTIMETER,
                icon="mdi:numeric-1-box-multiple-outline",
            ),
            cv.Optional(CONF_SET_POSITION2): number.number_schema(
                DeskPositionNumber,
                unit_of_measurement=UNIT_CENTIMETER,
                icon="mdi:numeric-2-box-multiple-outline",
            ),
            cv.Optional(CONF_SAVE_POSITION1_COMMAND, default=0x03): cv.hex_uint8_t,
            cv.Optional(CONF_SAVE_POSITION2_COMMAND, default=0x04): cv.hex_uint8_t,
            cv.Optional(CONF_REFRESH_POSITIONS): button.button_schema(
                DeskButton, icon="mdi:refresh"
            ),
            cv.Optional(CONF_POSITION1_HEIGHT): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:numeric-1-box-outline",
            ),
            cv.Optional(CONF_POSITION2_HEIGHT): sensor.sensor_schema(
                unit_of_measurement=UNIT_CENTIMETER,
                accuracy_decimals=1,
                icon="mdi:numeric-2-box-outline",
            ),
            cv.Optional(CONF_POSITION1_REPORT, default=0x25): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION2_REPORT, default=0x26): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION1_COMMAND, default=0x05): cv.hex_uint8_t,
            cv.Optional(CONF_POSITION2_COMMAND, default=0x06): cv.hex_uint8_t,
            cv.Optional(CONF_TARGET_HEIGHT): number.number_schema(
                DeskHeightNumber,
                unit_of_measurement=UNIT_CENTIMETER,
                icon="mdi:human-male-height-variant",
            ),
            cv.Optional(
                CONF_MOVE_DURATION, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STOP_EARLY, default=5): cv.int_range(min=0, max=50),
            cv.Optional(CONF_MIN_HEIGHT, default=50.0): cv.float_range(
                min=0, max=300
            ),
            cv.Optional(CONF_MAX_HEIGHT, default=140.0): cv.float_range(
                min=0, max=300
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "homemax_desk", baud_rate=9600, require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_move_duration(config[CONF_MOVE_DURATION]))
    cg.add(var.set_stop_early(config[CONF_STOP_EARLY]))
    cg.add(
        var.set_position_commands(
            config[CONF_POSITION1_COMMAND], config[CONF_POSITION2_COMMAND]
        )
    )
    cg.add(var.set_height_limits(config[CONF_MIN_HEIGHT], config[CONF_MAX_HEIGHT]))

    cg.add(
        var.set_position_reports(
            config[CONF_POSITION1_REPORT], config[CONF_POSITION2_REPORT]
        )
    )

    cg.add(
        var.set_save_commands(
            config[CONF_SAVE_POSITION1_COMMAND], config[CONF_SAVE_POSITION2_COMMAND]
        )
    )

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

    if CONF_POSITION1_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_POSITION1_HEIGHT])
        cg.add(var.set_position1_sensor(sens))

    if CONF_POSITION2_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_POSITION2_HEIGHT])
        cg.add(var.set_position2_sensor(sens))

    if CONF_HEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_HEIGHT])
        cg.add(var.set_height_sensor(sens))

    for key, action in BUTTON_ACTIONS.items():
        if key in config:
            btn = await button.new_button(config[key])
            cg.add(btn.set_parent(var))
            cg.add(btn.set_action(action))

    if CONF_TARGET_HEIGHT in config:
        num = await number.new_number(
            config[CONF_TARGET_HEIGHT],
            min_value=config[CONF_MIN_HEIGHT],
            max_value=config[CONF_MAX_HEIGHT],
            step=0.5,
        )
        cg.add(num.set_parent(var))
        cg.add(var.set_target_number(num))
