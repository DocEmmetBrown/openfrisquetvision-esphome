#include "zone_text.h"

#ifdef USE_TEXT
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.zone_text";

void ZoneProgramText::control(const std::string &value) {
  if (this->parent_ == nullptr) return;
  if (!this->parent_->request_zone_program(value)) {
    ESP_LOGW(TAG, "Programme refused (block unknown or not yet confirmed, a write in progress, or not 84 hexadecimal characters)");
    this->parent_->republish_zone_block();
    return;
  }
  ESP_LOGI(TAG, "Programme queued; shown once the boiler relays the accepted block");
}

void ZoneProgramText::dump_config() { LOG_TEXT("", "Frisquet zone programme", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
