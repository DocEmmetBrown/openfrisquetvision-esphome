#include "frisquet_select.h"

#ifdef USE_SELECT
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.select";

void FrisquetSelect::control(const std::string &value) {
  if (this->parent_ == nullptr) return;
  const bool zone = this->kind_ == kZone1Mode;
  const bool accepted = zone ? this->parent_->request_zone_mode(value) : this->parent_->request_hot_water_mode(value);
  if (!accepted) {
    // Nothing was queued: keep showing what the boiler really has.
    ESP_LOGW(TAG, "'%s' refused (not writable yet, a write in progress, or unknown)", value.c_str());
    if (zone) this->parent_->republish_zone_block();
    else this->parent_->republish_hot_water_mode();
    return;
  }
  ESP_LOGI(TAG, "'%s' queued; the select shows the boiler's value once it is confirmed", value.c_str());
}

void FrisquetSelect::dump_config() { LOG_SELECT("", "Frisquet select", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
