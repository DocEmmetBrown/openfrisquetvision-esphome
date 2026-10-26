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

}  // namespace frisquet_radio
}  // namespace esphome
#endif
