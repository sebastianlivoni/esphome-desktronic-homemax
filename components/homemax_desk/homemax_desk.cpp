#include "homemax_desk.h"
#include "esphome/core/log.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace homemax_desk {

static const char *const TAG = "homemax_desk";

// Commands verified on the Desktronic HomeMax (JCP35N12 controller)
static const uint8_t CMD_STOP[] = {0xF1, 0xF1, 0x2B, 0x00, 0x2B, 0x7E};
static const uint8_t CMD_GOTO_HEIGHT = 0x1B;  // + 2 bytes height in mm, verified
static const uint8_t REQ_CMD_SETTINGS = 0x07;
static const uint8_t REQ_CMD_LIMITS = 0x0C;
static const uint8_t REQ_CMD_POLL = 0x20;

// Controller message types
static const uint8_t MSG_HEIGHT = 0x01;
static const uint8_t MSG_LIMITS = 0x07;       // reply to 0x0C: max(2) min(2)
static const uint8_t MSG_USER_LIMITS = 0x20;  // reply to 0x20: which user limits are set
static const uint8_t MSG_USER_MAX = 0x21;     // user maximum height
static const uint8_t MSG_USER_MIN = 0x22;     // user minimum height

// A move counts as ended when the height hasn't changed for this long
static const uint32_t NO_CHANGE_TIMEOUT_MS = 3000;
// A move counts as reached within this distance of the target...
static const int GOTO_TOLERANCE_MM = 10;
// ...once the height has been stable this long
static const uint32_t GOTO_SETTLE_MS = 500;
// Longest full travel is ~15 s; give up (and send Stop) after this
static const uint32_t GOTO_TIMEOUT_MS = 60000;
// Let the desk settle after reaching a target before saving a position
static const uint32_t SAVE_SETTLE_MS = 1000;
// Desk counts as moving until the height has been stable this long
static const uint32_t MOVING_HOLD_MS = 1000;
// Gap between requests, so the controller can answer each one
static const uint32_t REQUEST_GAP_MS = 150;
// Controller counts as disconnected this long after a missed keep-alive
static const uint32_t CONNECTION_GRACE_MS = 10000;
// Hysteresis for the standing sensor (mm)
static const int STANDING_HYSTERESIS = 10;
// How often to publish the standing time while standing
static const uint32_t STANDING_PUBLISH_MS = 60000;

void HomeMaxDesk::setup() {
  this->last_tick_ = millis();
  if (this->moving_sensor_ != nullptr)
    this->moving_sensor_->publish_initial_state(false);
  this->publish_standing_time_();

  // Ask for positions and limits once the controller had time to start
  this->requests_ |= REQ_SETTINGS | REQ_LIMITS | REQ_POLL;
  this->settings_pending_ = false;
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

  // 2. Derived states
  this->update_moving_(now);
  this->update_posture_(now);
  this->update_connected_(now);

  // 3. After every move, once the desk is quiet, read positions (and limits) again
  if (this->mode_ == Mode::IDLE && this->settings_pending_ &&
      (int32_t) (now - this->settings_not_before_) >= 0 && now - this->last_change_ > 1000) {
    this->settings_pending_ = false;
    this->requests_ |= REQ_SETTINGS;
    if (this->auto_limits_ && !this->limits_known_)
      this->requests_ |= REQ_LIMITS;
  }

  // 4. Read the user limits regularly; this also notices a disconnected controller
  if (this->poll_interval_ms_ > 0 && this->mode_ == Mode::IDLE &&
      now - this->last_poll_ > this->poll_interval_ms_) {
    this->last_poll_ = now;
    this->requests_ |= REQ_POLL;
  }

  // 5. Send queued requests
  this->process_requests_(now);

  // 6. Save a position once the desk has settled at its new height
  if (this->scheduled_save_ != 0 && this->mode_ == Mode::IDLE &&
      (int32_t) (now - this->save_at_) >= 0) {
    const uint8_t pos = this->scheduled_save_;
    this->scheduled_save_ = 0;
    this->save_position(pos);
  }

  // 7. While moving, watch the height until the desk has arrived
  if (this->mode_ == Mode::GOTO)
    this->check_goto_(now);
}

void HomeMaxDesk::dump_config() {
  ESP_LOGCONFIG(TAG, "HomeMax Desk:");
  ESP_LOGCONFIG(TAG, "  Height range: %.1f - %.1f cm (%s)", this->min_height_ / 10.0f,
                this->max_height_ / 10.0f,
                !this->auto_limits_ ? "configured"
                                    : (this->limits_known_ ? "from controller" : "waiting for controller"));
  ESP_LOGCONFIG(TAG, "  Standing from: %.1f cm", this->standing_height_ / 10.0f);
  ESP_LOGCONFIG(TAG, "  Keep-alive: %u s", (unsigned) (this->poll_interval_ms_ / 1000));
  ESP_LOGCONFIG(TAG, "  Position commands: 0x%02X, 0x%02X", this->position_cmd_[0],
                this->position_cmd_[1]);
  ESP_LOGCONFIG(TAG, "  Save commands: 0x%02X, 0x%02X", this->save_cmd_[0], this->save_cmd_[1]);
  ESP_LOGCONFIG(TAG, "  Position reports: 0x%02X, 0x%02X, 0x%02X, 0x%02X", this->position_report_[0],
                this->position_report_[1], this->position_report_[2], this->position_report_[3]);
  LOG_SENSOR("  ", "Height", this->height_sensor_);
  LOG_SENSOR("  ", "Height percent", this->height_percent_sensor_);
  LOG_SENSOR("  ", "User minimum", this->user_min_sensor_);
  LOG_SENSOR("  ", "User maximum", this->user_max_sensor_);
  LOG_SENSOR("  ", "Standing time", this->standing_time_sensor_);
  LOG_BINARY_SENSOR("  ", "Moving", this->moving_sensor_);
  LOG_BINARY_SENSOR("  ", "Standing", this->standing_sensor_);
  LOG_BINARY_SENSOR("  ", "Controller connected", this->connected_sensor_);
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
  this->last_rx_ = millis();
  this->handle_frame_(f[2], f + 4, len);
}

void HomeMaxDesk::handle_frame_(uint8_t type, const uint8_t *data, uint8_t len) {
  // Current height: F2 F2 01 03 HH LL XX CS 7E (height in mm)
  if (type == MSG_HEIGHT && len >= 2) {
    this->on_height_((data[0] << 8) | data[1]);
    return;
  }

  // Physical limits: F2 F2 07 04 MAXH MAXL MINH MINL CS 7E
  if (type == MSG_LIMITS && len == 4) {
    this->on_physical_limits_((data[0] << 8) | data[1], (data[2] << 8) | data[3]);
    return;
  }

  // User limits: F2 F2 20 01 FLAGS CS 7E, also our keep-alive reply
  if (type == MSG_USER_LIMITS && len >= 1) {
    this->on_user_limit_flags_(data[0]);
    return;
  }
  // User maximum / minimum: F2 F2 21|22 02 HH LL CS 7E
  if ((type == MSG_USER_MAX || type == MSG_USER_MIN) && len >= 2) {
    this->on_user_limit_(type == MSG_USER_MAX, (data[0] << 8) | data[1]);
    return;
  }

  // Stored memory positions (height in mm, 0 = not set)
  for (uint8_t i = 0; i < NUM_POSITIONS; i++) {
    if (type == this->position_report_[i] && len >= 2) {
      const int h = (data[0] << 8) | data[1];
      ESP_LOGD(TAG, "Position %u: %.1f cm", i + 1, h / 10.0f);
      const float value = h > 0 ? h / 10.0f : NAN;
      if (i < NUM_CONTROLLABLE && this->position_sensor_[i] != nullptr)
        this->position_sensor_[i]->publish_state(value);
      if (i < NUM_CONTROLLABLE && this->position_number_[i] != nullptr && h > 0)
        this->position_number_[i]->publish_state(value);
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
  this->update_posture_(now);
  this->publish_height_derived_();
}

void HomeMaxDesk::on_physical_limits_(int max_h, int min_h) {
  if (min_h <= 0 || max_h <= min_h) {
    ESP_LOGW(TAG, "Ignoring implausible physical limits: min %d, max %d", min_h, max_h);
    return;
  }
  if (this->limits_known_ && min_h == this->physical_min_ && max_h == this->physical_max_)
    return;
  ESP_LOGI(TAG, "Physical height range: %.1f - %.1f cm", min_h / 10.0f, max_h / 10.0f);
  this->limits_known_ = true;
  this->physical_min_ = min_h;
  this->physical_max_ = max_h;
  this->apply_limits_();
}

// Reply to 0x20: which user limits are set.
// Low nibble != 0: a maximum is set, high nibble != 0: a minimum is set.
void HomeMaxDesk::on_user_limit_flags_(uint8_t flags) {
  const bool max_set = (flags & 0x0F) != 0;
  const bool min_set = (flags & 0xF0) != 0;
  ESP_LOGV(TAG, "User limit flags 0x%02X (min %s, max %s)", flags, min_set ? "set" : "off",
           max_set ? "set" : "off");

  bool changed = false;
  if (!max_set && this->user_max_ != -1) {
    this->user_max_ = -1;
    changed = true;
  }
  if (!min_set && this->user_min_ != -1) {
    this->user_min_ = -1;
    changed = true;
  }
  // Publish "not set" once, so the sensors don't stay unknown
  if (!max_set && this->user_max_sensor_ != nullptr && !this->user_max_sensor_->has_state())
    this->user_max_sensor_->publish_state(NAN);
  if (!min_set && this->user_min_sensor_ != nullptr && !this->user_min_sensor_->has_state())
    this->user_min_sensor_->publish_state(NAN);

  if (changed) {
    ESP_LOGI(TAG, "User height limit removed");
    this->apply_limits_();
  }
}

// 0x21: user maximum, 0x22: user minimum (height in mm)
void HomeMaxDesk::on_user_limit_(bool is_max, int h) {
  int &slot = is_max ? this->user_max_ : this->user_min_;
  if (h <= 0 || h == slot)
    return;
  slot = h;
  ESP_LOGI(TAG, "User height %s: %.1f cm", is_max ? "maximum" : "minimum", h / 10.0f);
  this->apply_limits_();
}

// Work out the range the desk can actually use: user limits where set,
// otherwise the physical limits, otherwise min_height / max_height.
void HomeMaxDesk::apply_limits_() {
  if (this->user_min_sensor_ != nullptr)
    this->user_min_sensor_->publish_state(this->user_min_ > 0 ? this->user_min_ / 10.0f : NAN);
  if (this->user_max_sensor_ != nullptr)
    this->user_max_sensor_->publish_state(this->user_max_ > 0 ? this->user_max_ / 10.0f : NAN);

  int min_h = this->config_min_, max_h = this->config_max_;
  if (this->auto_limits_) {
    if (this->physical_min_ > 0)
      min_h = this->physical_min_;
    if (this->physical_max_ > 0)
      max_h = this->physical_max_;
    if (this->user_min_ > 0)
      min_h = this->user_min_;
    if (this->user_max_ > 0)
      max_h = this->user_max_;
  }
  if (max_h <= min_h) {
    ESP_LOGW(TAG, "Inconsistent limits (%.1f - %.1f cm), keeping %.1f - %.1f cm", min_h / 10.0f,
             max_h / 10.0f, this->min_height_ / 10.0f, this->max_height_ / 10.0f);
    return;
  }

  this->min_height_ = min_h;
  this->max_height_ = max_h;
  ESP_LOGD(TAG, "Usable height range: %.1f - %.1f cm", min_h / 10.0f, max_h / 10.0f);
  if (this->height_min_sensor_ != nullptr)
    this->height_min_sensor_->publish_state(min_h / 10.0f);
  if (this->height_max_sensor_ != nullptr)
    this->height_max_sensor_->publish_state(max_h / 10.0f);

  // Narrow the number entities to the usable range. Home Assistant picks up
  // the new range the next time it connects to the device.
  const float min_cm = min_h / 10.0f, max_cm = max_h / 10.0f;
  if (this->target_number_ != nullptr) {
    this->target_number_->traits.set_min_value(min_cm);
    this->target_number_->traits.set_max_value(max_cm);
  }
  for (auto *n : this->position_number_) {
    if (n != nullptr) {
      n->traits.set_min_value(min_cm);
      n->traits.set_max_value(max_cm);
    }
  }
  this->publish_height_derived_();
}

// ---------------------------------------------------------------------------
// Derived states
// ---------------------------------------------------------------------------

float HomeMaxDesk::get_height_percent() const {
  if (this->current_height_ < 0 || this->max_height_ <= this->min_height_)
    return NAN;
  const float pct = 100.0f * (this->current_height_ - this->min_height_) /
                    (float) (this->max_height_ - this->min_height_);
  return clamp(pct, 0.0f, 100.0f);
}

void HomeMaxDesk::publish_height_derived_() {
  if (this->height_percent_sensor_ != nullptr && this->current_height_ >= 0)
    this->height_percent_sensor_->publish_state(this->get_height_percent());
  this->publish_cover_();
}

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

void HomeMaxDesk::update_posture_(uint32_t now) {
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
  if (standing == this->standing_ && this->posture_known_)
    return;

  this->standing_ = standing;
  this->posture_known_ = true;
  ESP_LOGD(TAG, "Posture: %s", standing ? "standing" : "sitting");
  if (this->standing_sensor_ != nullptr)
    this->standing_sensor_->publish_state(standing);
  this->publish_standing_time_();
}

void HomeMaxDesk::update_connected_(uint32_t now) {
  if (this->poll_interval_ms_ == 0)
    return;
  bool connected;
  if (this->last_rx_ == 0) {
    // Nothing received yet: give the controller some time after boot
    if (now < 15000)
      return;
    connected = false;
  } else {
    connected = now - this->last_rx_ <= this->poll_interval_ms_ + CONNECTION_GRACE_MS;
  }
  if (connected == this->connected_ && this->connected_published_)
    return;

  this->connected_ = connected;
  this->connected_published_ = true;
  if (connected) {
    ESP_LOGI(TAG, "Controller connected");
  } else {
    ESP_LOGW(TAG, "No answer from controller, check the cable");
  }
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_state(connected);
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

  const float pos = this->get_height_percent() / 100.0f;

  cover::CoverOperation op = cover::COVER_OPERATION_IDLE;
  if (this->moving_) {
    int8_t dir = this->direction_;
    if (this->mode_ == Mode::GOTO && this->target_ >= 0)
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
  this->requests_ |= REQ_SETTINGS | REQ_POLL;
  if (this->auto_limits_)
    this->requests_ |= REQ_LIMITS;
}

void HomeMaxDesk::process_requests_(uint32_t now) {
  if (this->requests_ == 0 || this->mode_ != Mode::IDLE || now < 3000 ||
      now - this->last_request_ < REQUEST_GAP_MS)
    return;
  this->last_request_ = now;

  if (this->requests_ & REQ_SETTINGS) {
    this->requests_ &= ~REQ_SETTINGS;
    ESP_LOGD(TAG, "Requesting stored positions");
    this->send_command(REQ_CMD_SETTINGS);
  } else if (this->requests_ & REQ_LIMITS) {
    this->requests_ &= ~REQ_LIMITS;
    ESP_LOGD(TAG, "Requesting height limits");
    this->send_command(REQ_CMD_LIMITS);
  } else if (this->requests_ & REQ_POLL) {
    this->requests_ &= ~REQ_POLL;
    ESP_LOGV(TAG, "Requesting user limits");
    this->send_command(REQ_CMD_POLL);
  }
}

void HomeMaxDesk::send_goto_(int target_mm) {
  const uint8_t hi = (target_mm >> 8) & 0xFF;
  const uint8_t lo = target_mm & 0xFF;
  const uint8_t cs = CMD_GOTO_HEIGHT + 0x02 + hi + lo;
  const uint8_t buf[] = {0xF1, 0xF1, CMD_GOTO_HEIGHT, 0x02, hi, lo, cs, 0x7E};
  this->write_array(buf, sizeof(buf));
}

void HomeMaxDesk::check_goto_(uint32_t now) {
  const bool known = this->current_height_ >= 0;
  const bool at_target = known && std::abs(this->target_ - this->current_height_) <= GOTO_TOLERANCE_MM;

  if (at_target && now - this->height_changed_at_ >= GOTO_SETTLE_MS) {
    this->end_goto_(true, "Target height reached");
    return;
  }
  if (now - this->last_change_ > NO_CHANGE_TIMEOUT_MS) {
    // The desk stopped before the target: limit reached, handset used, or ignored
    const bool moved = known && (int32_t) (this->height_changed_at_ - this->mode_start_) > 0;
    this->end_goto_(at_target, moved ? "Desk stopped before the target" : "Desk didn't move");
    return;
  }
  if (now - this->mode_start_ > GOTO_TIMEOUT_MS) {
    this->scheduled_save_ = 0;
    this->finish_("Timeout, stopping");
  }
}

void HomeMaxDesk::end_goto_(bool reached, const char *reason) {
  // The controller stops by itself, so no Stop command here
  const uint8_t save = this->pending_save_;
  ESP_LOGI(TAG, "%s", reason);
  this->mode_ = Mode::IDLE;
  this->target_ = -1;
  this->pending_save_ = 0;
  if (reached && save != 0) {
    this->scheduled_save_ = save;
    this->save_at_ = millis() + SAVE_SETTLE_MS;
  }
  this->publish_cover_();
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
  // Move all the way up; the controller stops at the top or on Stop
  ESP_LOGD(TAG, "Move up");
  this->goto_height(this->max_height_ / 10.0f);
}

void HomeMaxDesk::move_down() {
  // Move all the way down; the controller stops at the bottom or on Stop
  ESP_LOGD(TAG, "Move down");
  this->goto_height(this->min_height_ / 10.0f);
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
  this->mode_start_ = now;
  this->last_change_ = now;
  ESP_LOGI(TAG, "Moving to %.1f cm", target / 10.0f);

  // The controller moves to the height by itself
  this->mode_ = Mode::GOTO;
  this->send_goto_(target);
  this->publish_cover_();
}

void HomeMaxDesk::goto_position(uint8_t position) {
  if (position < 1 || position > NUM_CONTROLLABLE) {
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
  if (position < 1 || position > NUM_CONTROLLABLE) {
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
  if (position < 1 || position > NUM_CONTROLLABLE) {
    ESP_LOGW(TAG, "Unknown position %u", position);
    return;
  }
  ESP_LOGI(TAG, "Setting position %u to %.1f cm", position, cm);
  this->goto_height(cm);
  if (this->mode_ == Mode::GOTO)
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
