#include "homemax_desk.h"
#include "esphome/core/log.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace homemax_desk {

static const char *const TAG = "homemax_desk";

// Commands verified on the Desktronic HomeMax
static const uint8_t CMD_UP[] = {0xF1, 0xF1, 0x01, 0x00, 0x01, 0x7E};
static const uint8_t CMD_DOWN[] = {0xF1, 0xF1, 0x02, 0x00, 0x02, 0x7E};
static const uint8_t CMD_STOP[] = {0xF1, 0xF1, 0x2B, 0x00, 0x2B, 0x7E};

// The controller only keeps moving while commands keep arriving
static const uint32_t SEND_INTERVAL_MS = 100;
// Safety limits for go-to-height
static const uint32_t NO_CHANGE_TIMEOUT_MS = 3000;
static const uint32_t TARGET_TIMEOUT_MS = 30000;
// How long to wait for a height reading after the wake-up nudge
static const uint32_t WAKE_TIMEOUT_MS = 1500;
// Let the desk settle after reaching a target before saving a position
static const uint32_t SAVE_SETTLE_MS = 1000;
// Desk counts as moving until the height has been stable this long
static const uint32_t MOVING_HOLD_MS = 1000;
// Hysteresis for the standing sensor (mm)
static const int STANDING_HYSTERESIS = 10;
// How often to publish the standing time while standing
static const uint32_t STANDING_PUBLISH_MS = 60000;

void HomeMaxDesk::setup() {
  this->last_tick_ = millis();
  if (this->moving_sensor_ != nullptr)
    this->moving_sensor_->publish_initial_state(false);
  this->publish_standing_time_();
}

void HomeMaxDesk::loop() {
  // 1. Read everything the controller sent
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c))
      break;
    this->handle_byte_(c);
  }

  const uint32_t now = millis();

  // 2. Moving and standing state
  this->update_moving_(now);
  this->update_standing_(now);

  // 3. Ask for the stored positions at startup and after every move,
  //    once the desk has been quiet for a moment
  if (this->mode_ == Mode::IDLE && this->settings_pending_ &&
      (int32_t) (now - this->settings_not_before_) >= 0 && now - this->last_change_ > 1000) {
    this->settings_pending_ = false;
    this->request_settings();
  }

  // 4. Save a position once the desk has settled at its new height
  if (this->scheduled_save_ != 0 && this->mode_ == Mode::IDLE &&
      (int32_t) (now - this->save_at_) >= 0) {
    const uint8_t pos = this->scheduled_save_;
    this->scheduled_save_ = 0;
    this->save_position(pos);
  }

  // 5. Keep sending movement commands while moving
  if (this->mode_ == Mode::IDLE)
    return;

  if (!this->send_now_ && now - this->last_send_ < SEND_INTERVAL_MS)
    return;
  this->send_now_ = false;
  this->last_send_ = now;

  switch (this->mode_) {
    case Mode::MANUAL_UP:
    case Mode::MANUAL_DOWN:
      if ((int32_t) (now - this->manual_until_) >= 0) {
        this->finish_("Manual move finished");
        return;
      }
      this->send_(this->mode_ == Mode::MANUAL_UP ? CMD_UP : CMD_DOWN);
      break;

    case Mode::TARGET: {
      if (this->current_height_ < 0) {
        // Waiting for the first height reading after the nudge
        if (now - this->mode_start_ > WAKE_TIMEOUT_MS)
          this->finish_("No height received from controller, cancelled");
        return;
      }
      const int diff = this->target_ - this->current_height_;
      if (std::abs(diff) <= this->stop_early_) {
        const uint8_t save = this->pending_save_;
        this->finish_("Target height reached");
        if (save != 0) {
          this->scheduled_save_ = save;
          this->save_at_ = now + SAVE_SETTLE_MS;
        }
        return;
      }
      if (now - this->last_change_ > NO_CHANGE_TIMEOUT_MS) {
        this->finish_("Height not changing, stopping (limit reached?)");
        return;
      }
      if (now - this->mode_start_ > TARGET_TIMEOUT_MS) {
        this->finish_("Timeout, stopping");
        return;
      }
      this->send_(diff > 0 ? CMD_UP : CMD_DOWN);
      break;
    }

    default:
      break;
  }
}

void HomeMaxDesk::dump_config() {
  ESP_LOGCONFIG(TAG, "HomeMax Desk:");
  ESP_LOGCONFIG(TAG, "  Move duration: %u ms", (unsigned) this->move_duration_);
  ESP_LOGCONFIG(TAG, "  Stop early: %d mm", this->stop_early_);
  ESP_LOGCONFIG(TAG, "  Height range: %.1f - %.1f cm", this->min_height_ / 10.0f,
                this->max_height_ / 10.0f);
  ESP_LOGCONFIG(TAG, "  Standing from: %.1f cm", this->standing_height_ / 10.0f);
  ESP_LOGCONFIG(TAG, "  Position commands: 0x%02X, 0x%02X", this->position_cmd_[0],
                this->position_cmd_[1]);
  ESP_LOGCONFIG(TAG, "  Save commands: 0x%02X, 0x%02X", this->save_cmd_[0], this->save_cmd_[1]);
  ESP_LOGCONFIG(TAG, "  Position reports: 0x%02X, 0x%02X", this->position_report_[0],
                this->position_report_[1]);
  LOG_SENSOR("  ", "Height", this->height_sensor_);
  LOG_SENSOR("  ", "Position 1", this->position_sensor_[0]);
  LOG_SENSOR("  ", "Position 2", this->position_sensor_[1]);
  LOG_SENSOR("  ", "Standing time", this->standing_time_sensor_);
  LOG_BINARY_SENSOR("  ", "Moving", this->moving_sensor_);
  LOG_BINARY_SENSOR("  ", "Standing", this->standing_sensor_);
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

// Controller messages: F2 F2 TYPE LEN DATA[LEN] CS 7E
// CS = (TYPE + LEN + DATA...) & 0xFF
void HomeMaxDesk::handle_byte_(uint8_t c) {
  // Wait for the two start bytes
  if (this->rx_len_ < 2) {
    if (c == 0xF2) {
      this->rx_buf_[this->rx_len_++] = c;
    } else {
      this->rx_len_ = 0;
    }
    return;
  }
  // A third F2 right after the start: treat it as a new start
  if (this->rx_len_ == 2 && c == 0xF2)
    return;

  this->rx_buf_[this->rx_len_++] = c;
  if (this->rx_len_ < 4)
    return;

  const uint8_t len = this->rx_buf_[3];
  const uint8_t total = len + 6;  // start(2) + type + len + data + cs + end
  if (total > RX_MAX) {
    this->rx_len_ = 0;
    return;
  }
  if (this->rx_len_ < total)
    return;

  // Complete frame received
  this->rx_len_ = 0;
  const uint8_t *f = this->rx_buf_;
  if (f[total - 1] != 0x7E) {
    ESP_LOGV(TAG, "Frame without end byte, dropped");
    return;
  }
  uint8_t sum = 0;
  for (uint8_t i = 2; i < total - 2; i++)
    sum += f[i];
  if (sum != f[total - 2]) {
    ESP_LOGW(TAG, "Checksum mismatch in message type 0x%02X", f[2]);
    return;
  }
  this->handle_frame_(f[2], f + 4, len);
}

void HomeMaxDesk::handle_frame_(uint8_t type, const uint8_t *data, uint8_t len) {
  // Current height: F2 F2 01 03 HH LL XX CS 7E (height in mm)
  if (type == 0x01 && len >= 2) {
    this->on_height_((data[0] << 8) | data[1]);
    return;
  }

  // Stored memory positions (height in mm)
  for (uint8_t i = 0; i < 2; i++) {
    if (type == this->position_report_[i] && len >= 2) {
      const int h = (data[0] << 8) | data[1];
      ESP_LOGD(TAG, "Position %u: %.1f cm", i + 1, h / 10.0f);
      if (this->position_sensor_[i] != nullptr)
        this->position_sensor_[i]->publish_state(h / 10.0f);
      if (this->position_number_[i] != nullptr)
        this->position_number_[i]->publish_state(h / 10.0f);
      return;
    }
  }

  // Log everything else so unknown messages can be identified
  char hex[3 * 16 + 1] = {0};
  for (uint8_t i = 0; i < len && i < 16; i++)
    sprintf(hex + i * 3, "%02X ", data[i]);
  ESP_LOGD(TAG, "Message type 0x%02X, %u bytes: %s", type, len, hex);
}

void HomeMaxDesk::on_height_(int h) {
  if (h == this->current_height_)
    return;

  const uint32_t now = millis();
  if (this->current_height_ >= 0)
    this->direction_ = h > this->current_height_ ? 1 : -1;
  this->current_height_ = h;
  this->last_change_ = now;
  this->height_changed_at_ = now;
  this->settings_pending_ = true;  // refresh positions after the move

  ESP_LOGD(TAG, "Height: %.1f cm", h / 10.0f);
  if (this->height_sensor_ != nullptr)
    this->height_sensor_->publish_state(h / 10.0f);

  this->update_moving_(now);
  this->update_standing_(now);
  this->publish_cover_();
}

// ---------------------------------------------------------------------------
// Derived states
// ---------------------------------------------------------------------------

void HomeMaxDesk::update_moving_(uint32_t now) {
  const bool height_changing =
      this->current_height_ >= 0 && now - this->height_changed_at_ < MOVING_HOLD_MS;
  const bool moving = this->mode_ != Mode::IDLE || height_changing;
  if (moving == this->moving_)
    return;

  this->moving_ = moving;
  if (!moving)
    this->direction_ = 0;
  ESP_LOGD(TAG, "Moving: %s", moving ? "yes" : "no");
  if (this->moving_sensor_ != nullptr)
    this->moving_sensor_->publish_state(moving);
  this->publish_cover_();
}

void HomeMaxDesk::update_standing_(uint32_t now) {
  // Count standing time
  const uint32_t elapsed = now - this->last_tick_;
  this->last_tick_ = now;
  if (this->standing_)
    this->standing_ms_ += elapsed;

  if (this->standing_ && now - this->last_time_publish_ >= STANDING_PUBLISH_MS)
    this->publish_standing_time_();

  // Update the sitting/standing state with a little hysteresis
  if (this->current_height_ < 0)
    return;
  bool standing = this->standing_;
  if (this->current_height_ >= this->standing_height_) {
    standing = true;
  } else if (this->current_height_ < this->standing_height_ - STANDING_HYSTERESIS) {
    standing = false;
  }
  if (standing == this->standing_ && this->standing_known_)
    return;

  this->standing_ = standing;
  this->standing_known_ = true;
  ESP_LOGD(TAG, "Posture: %s", standing ? "standing" : "sitting");
  if (this->standing_sensor_ != nullptr)
    this->standing_sensor_->publish_state(standing);
  this->publish_standing_time_();
}

void HomeMaxDesk::publish_standing_time_() {
  this->last_time_publish_ = millis();
  if (this->standing_time_sensor_ != nullptr)
    this->standing_time_sensor_->publish_state(this->standing_ms_ / 60000.0f);
}

void HomeMaxDesk::reset_standing_time() {
  ESP_LOGI(TAG, "Resetting standing time (was %.1f min)", this->standing_ms_ / 60000.0f);
  this->standing_ms_ = 0;
  this->publish_standing_time_();
}

void HomeMaxDesk::publish_cover_() {
  if (this->cover_ == nullptr || this->current_height_ < 0)
    return;

  const float range = (float) (this->max_height_ - this->min_height_);
  float pos = range > 0 ? (this->current_height_ - this->min_height_) / range : 0.0f;
  pos = clamp(pos, 0.0f, 1.0f);

  cover::CoverOperation op = cover::COVER_OPERATION_IDLE;
  if (this->moving_) {
    int8_t dir = this->direction_;
    if (this->mode_ == Mode::MANUAL_UP)
      dir = 1;
    else if (this->mode_ == Mode::MANUAL_DOWN)
      dir = -1;
    else if (this->mode_ == Mode::TARGET && this->current_height_ >= 0)
      dir = this->target_ > this->current_height_ ? 1 : -1;
    if (dir > 0)
      op = cover::COVER_OPERATION_OPENING;
    else if (dir < 0)
      op = cover::COVER_OPERATION_CLOSING;
  }

  if (this->cover_->position == pos && this->cover_->current_operation == op)
    return;
  this->cover_->position = pos;
  this->cover_->current_operation = op;
  this->cover_->publish_state(false);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void HomeMaxDesk::send_(const uint8_t *cmd) { this->write_array(cmd, 6); }

void HomeMaxDesk::send_command(uint8_t cmd) {
  const uint8_t buf[] = {0xF1, 0xF1, cmd, 0x00, cmd, 0x7E};
  this->write_array(buf, sizeof(buf));
}

void HomeMaxDesk::request_settings() {
  ESP_LOGD(TAG, "Requesting settings");
  this->send_command(0x07);
}

void HomeMaxDesk::finish_(const char *reason) {
  this->send_(CMD_STOP);
  ESP_LOGI(TAG, "%s", reason);
  this->mode_ = Mode::IDLE;
  this->target_ = -1;
  this->pending_save_ = 0;
  this->publish_cover_();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void HomeMaxDesk::move_up() {
  // Pressing again while already moving up extends the move
  this->scheduled_save_ = 0;
  this->pending_save_ = 0;
  this->mode_ = Mode::MANUAL_UP;
  this->manual_until_ = millis() + this->move_duration_;
  this->send_now_ = true;
  ESP_LOGD(TAG, "Move up");
}

void HomeMaxDesk::move_down() {
  this->scheduled_save_ = 0;
  this->pending_save_ = 0;
  this->mode_ = Mode::MANUAL_DOWN;
  this->manual_until_ = millis() + this->move_duration_;
  this->send_now_ = true;
  ESP_LOGD(TAG, "Move down");
}

void HomeMaxDesk::stop() {
  this->scheduled_save_ = 0;
  this->finish_("Stop");
}

void HomeMaxDesk::goto_height(float cm) {
  int target = (int) (cm * 10 + 0.5f);
  if (target < this->min_height_ || target > this->max_height_) {
    ESP_LOGW(TAG, "Target %.1f cm outside limits (%.1f - %.1f cm)", cm, this->min_height_ / 10.0f,
             this->max_height_ / 10.0f);
    return;
  }

  const uint32_t now = millis();
  this->scheduled_save_ = 0;
  this->pending_save_ = 0;
  this->target_ = target;
  this->mode_ = Mode::TARGET;
  this->mode_start_ = now;
  this->last_change_ = now;
  ESP_LOGI(TAG, "Moving to %.1f cm", cm);

  if (this->current_height_ < 0) {
    // The controller is silent when idle: nudge it so it reports its height
    ESP_LOGD(TAG, "Height unknown, nudging desk to get a reading");
    this->send_(CMD_UP);
    this->last_send_ = now;
  } else {
    this->send_now_ = true;
  }
}

void HomeMaxDesk::goto_position(uint8_t position) {
  if (position < 1 || position > 2) {
    ESP_LOGW(TAG, "Unknown position %u", position);
    return;
  }
  // Cancel our own movement; the controller moves to the position by itself
  this->mode_ = Mode::IDLE;
  this->target_ = -1;
  this->pending_save_ = 0;
  this->scheduled_save_ = 0;
  ESP_LOGI(TAG, "Go to position %u", position);
  this->send_command(this->position_cmd_[position - 1]);
}

void HomeMaxDesk::save_position(uint8_t position) {
  if (position < 1 || position > 2) {
    ESP_LOGW(TAG, "Unknown position %u", position);
    return;
  }
  if (this->mode_ != Mode::IDLE) {
    ESP_LOGW(TAG, "Desk is moving, not saving position %u", position);
    return;
  }
  if (this->current_height_ >= 0) {
    ESP_LOGI(TAG, "Saving %.1f cm as position %u", this->current_height_ / 10.0f, position);
    // Show the new value right away; the controller's report confirms it
    const float h = this->current_height_ / 10.0f;
    if (this->position_sensor_[position - 1] != nullptr)
      this->position_sensor_[position - 1]->publish_state(h);
    if (this->position_number_[position - 1] != nullptr)
      this->position_number_[position - 1]->publish_state(h);
  } else {
    ESP_LOGI(TAG, "Saving current height as position %u", position);
  }
  this->send_command(this->save_cmd_[position - 1]);

  // Read the stored positions back shortly afterwards
  this->settings_pending_ = true;
  this->settings_not_before_ = millis() + 500;
}

void HomeMaxDesk::set_position_height(uint8_t position, float cm) {
  if (position < 1 || position > 2) {
    ESP_LOGW(TAG, "Unknown position %u", position);
    return;
  }
  ESP_LOGI(TAG, "Setting position %u to %.1f cm", position, cm);
  this->goto_height(cm);
  if (this->mode_ == Mode::TARGET)
    this->pending_save_ = position;
}

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

void DeskButton::press_action() {
  switch (this->action_) {
    case ACTION_MOVE_UP:
      this->parent_->move_up();
      break;
    case ACTION_MOVE_DOWN:
      this->parent_->move_down();
      break;
    case ACTION_POSITION1:
      this->parent_->goto_position(1);
      break;
    case ACTION_POSITION2:
      this->parent_->goto_position(2);
      break;
    case ACTION_REFRESH_POSITIONS:
      this->parent_->request_settings();
      break;
    case ACTION_SAVE_POSITION1:
      this->parent_->save_position(1);
      break;
    case ACTION_SAVE_POSITION2:
      this->parent_->save_position(2);
      break;
    default:
      this->parent_->stop();
      break;
  }
}

void DeskHeightNumber::control(float value) {
  this->publish_state(value);
  this->parent_->goto_height(value);
}

void DeskPositionNumber::control(float value) {
  // The state is published once the position is actually saved
  this->parent_->set_position_height(this->slot_, value);
}

cover::CoverTraits DeskCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(false);
  return traits;
}

void DeskCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    this->parent_->stop();
    return;
  }
  if (call.get_position().has_value()) {
    const float pos = *call.get_position();
    const float min = this->parent_->get_min_height();
    const float max = this->parent_->get_max_height();
    this->parent_->goto_height(min + pos * (max - min));
  }
}

}  // namespace homemax_desk
}  // namespace esphome
