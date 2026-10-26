#include "hot_water_select.h"

#ifdef USE_SELECT
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.hot_water";

void HotWaterSelect::control(const std::string &value) {
  if (this->parent_ == nullptr) return;
  const bool accepted = this->parent_->request_hot_water_mode(value);
  if (!accepted) {
    // Nothing was queued: keep showing the mode the boiler really has.
    ESP_LOGW(TAG, "Hot-water mode '%s' refused (not writable yet, a write in progress, or unknown)", value.c_str());
    this->parent_->republish_hot_water_mode();
    return;
  }
  ESP_LOGI(TAG, "Hot-water mode '%s' queued; the select shows the boiler's mode once read back", value.c_str());
}

void HotWaterSelect::dump_config() { LOG_SELECT("", "Frisquet hot-water mode", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
