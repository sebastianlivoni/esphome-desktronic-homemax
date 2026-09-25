#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"

namespace esphome {
namespace homemax_desk {

// Must match BUTTON_ACTIONS in __init__.py
enum ButtonAction : uint8_t {
  ACTION_MOVE_UP = 0,
  ACTION_MOVE_DOWN = 1,
  ACTION_STOP = 2,
  ACTION_POSITION1 = 3,
  ACTION_POSITION2 = 4,
  ACTION_REFRESH_POSITIONS = 5,
  ACTION_SAVE_POSITION1 = 6,
  ACTION_SAVE_POSITION2 = 7,
};

class HomeMaxDesk : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Configuration (called from generated code)
  void set_height_sensor(sensor::Sensor *s) { this->height_sensor_ = s; }
  void set_target_number(number::Number *n) { this->target_number_ = n; }
  void set_position1_sensor(sensor::Sensor *s) { this->position_sensor_[0] = s; }
  void set_position2_sensor(sensor::Sensor *s) { this->position_sensor_[1] = s; }
  void set_position_number(uint8_t slot, number::Number *n) {
    if (slot >= 1 && slot <= 2)
      this->position_number_[slot - 1] = n;
  }
  void set_save_commands(uint8_t pos1, uint8_t pos2) {
    this->save_cmd_[0] = pos1;
    this->save_cmd_[1] = pos2;
  }
  void set_position_reports(uint8_t pos1, uint8_t pos2) {
    this->position_report_[0] = pos1;
    this->position_report_[1] = pos2;
  }
  void set_move_duration(uint32_t ms) { this->move_duration_ = ms; }
  void set_stop_early(int mm) { this->stop_early_ = mm; }
  void set_position_commands(uint8_t pos1, uint8_t pos2) {
    this->position_cmd_[0] = pos1;
    this->position_cmd_[1] = pos2;
  }
  void set_height_limits(float min_cm, float max_cm) {
    this->min_height_ = (int) (min_cm * 10 + 0.5f);
    this->max_height_ = (int) (max_cm * 10 + 0.5f);
  }

  // Actions (also usable from lambdas, e.g. id(desk).goto_height(80);)
  void move_up();
  void move_down();
  void stop();
  void goto_height(float cm);
  // Go to a memory position stored in the controller (1 or 2)
  void goto_position(uint8_t position);

  // Save the current height as memory position 1 or 2
  void save_position(uint8_t position);
  // Move to a height, then save it as memory position 1 or 2
  void set_position_height(uint8_t position, float cm);

  // Ask the controller to report its settings (incl. stored positions)
  void request_settings();

  // Send a raw single-byte Jiecang command without data:
  // F1 F1 <cmd> 00 <cmd> 7E
  void send_command(uint8_t cmd);

  // Current height in cm, or NAN if not known yet
  float get_height() const {
    return this->current_height_ < 0 ? NAN : this->current_height_ / 10.0f;
  }
  bool is_moving() const { return this->mode_ != Mode::IDLE; }

 protected:
  enum class Mode : uint8_t { IDLE, MANUAL_UP, MANUAL_DOWN, TARGET };

  void handle_byte_(uint8_t c);
  void handle_frame_(uint8_t type, const uint8_t *data, uint8_t len);
  void send_(const uint8_t *cmd);
  void finish_(const char *reason);

  sensor::Sensor *height_sensor_{nullptr};
  number::Number *target_number_{nullptr};
  sensor::Sensor *position_sensor_[2]{nullptr, nullptr};
  number::Number *position_number_[2]{nullptr, nullptr};

  uint32_t move_duration_{1000};
  int stop_early_{5};
  int min_height_{500};
  int max_height_{1400};
  uint8_t position_cmd_[2]{0x05, 0x06};
  uint8_t position_report_[2]{0x25, 0x26};
  uint8_t save_cmd_[2]{0x03, 0x04};

  Mode mode_{Mode::IDLE};
  int current_height_{-1};  // mm, -1 = unknown
  int target_{-1};          // mm
  uint32_t mode_start_{0};
  uint32_t manual_until_{0};
  uint32_t last_change_{0};
  uint32_t last_send_{0};
  bool send_now_{false};
  bool settings_pending_{true};  // request settings once the desk is quiet
  uint32_t settings_not_before_{3000};
  uint8_t pending_save_{0};        // position to save when target is reached
  uint8_t scheduled_save_{0};      // position to save at save_at_
  uint32_t save_at_{0};

  // Receive buffer for one frame: F2 F2 TYPE LEN DATA... CS 7E
  static const uint8_t RX_MAX = 32;
  uint8_t rx_buf_[RX_MAX]{};
  uint8_t rx_len_{0};
};

class DeskButton : public button::Button, public Parented<HomeMaxDesk> {
 public:
  void set_action(uint8_t action) { this->action_ = action; }

 protected:
  void press_action() override;
  uint8_t action_{ACTION_STOP};
};

class DeskHeightNumber : public number::Number, public Parented<HomeMaxDesk> {
 protected:
  void control(float value) override;
};

// Number that moves the desk to a height and saves it as a memory position
class DeskPositionNumber : public number::Number, public Parented<HomeMaxDesk> {
 public:
  void set_slot(uint8_t slot) { this->slot_ = slot; }

 protected:
  void control(float value) override;
  uint8_t slot_{1};
};

}  // namespace homemax_desk
}  // namespace esphome
