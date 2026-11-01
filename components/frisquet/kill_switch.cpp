#include "kill_switch.h"

#ifdef USE_SWITCH
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.kill_switch";

void KillSwitch::setup() {
  // After a reboot the emergency stop is engaged unless the YAML asks for another restore mode.
  const auto restored = this->get_initial_state_with_restore_mode();
  const bool engaged = restored.has_value() ? *restored : true;
  if (engaged) this->turn_on();
  else this->turn_off();
}

void KillSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) this->parent_->set_kill_switch(state);
  this->publish_state(state);
  ESP_LOGI(TAG, "Emergency stop %s", state ? "ENGAGED (no transmission)" : "released");
}

void KillSwitch::dump_config() { LOG_SWITCH("", "Frisquet emergency stop", this); }

void ZoneBoostSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) return;
  if (!this->parent_->request_zone_boost(state)) {
    // Nothing queued: keep showing the block's real state.
    ESP_LOGW(TAG, "Zone 1 boost %s refused (block not confirmed, not in comfort, or already %s)", state ? "on" : "off",
             state ? "on" : "off");
    this->parent_->republish_zone_block();
    return;
  }
  ESP_LOGI(TAG, "Zone 1 boost %s queued; the switch follows the zone block once confirmed", state ? "on" : "off");
}

void ZoneBoostSwitch::dump_config() { LOG_SWITCH("", "Frisquet zone 1 boost", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
