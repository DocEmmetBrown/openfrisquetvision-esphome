#include "zone_number.h"

#ifdef USE_NUMBER
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.zone_number";

void ZoneSetpointNumber::control(float value) {
  if (this->parent_ == nullptr) return;
  if (!this->parent_->request_zone_setpoint(this->field_, value)) {
    // Nothing was queued: keep showing the value the boiler really has.
    ESP_LOGW(TAG, "Zone setpoint %.1f refused (block not known yet, a write in progress, or inconsistent values)",
             value);
    this->parent_->republish_zone_setpoints();
    return;
  }
  ESP_LOGI(TAG, "Zone setpoint %.1f queued; shown once the boiler relays the accepted block", value);
}

void ZoneSetpointNumber::dump_config() { LOG_NUMBER("", "Frisquet zone setpoint", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
