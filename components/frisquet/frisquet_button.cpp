#include "frisquet_button.h"

#ifdef USE_BUTTON
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio.button";

void FrisquetButton::press_action() {
  if (this->parent_ == nullptr) return;
  switch (this->kind_) {
    case kRecoverNetwork: this->parent_->start_network_recovery(); break;
    case kAssociate: this->parent_->start_association(); break;
    case kForgetNetwork: this->parent_->forget_network(); break;
  }
}

void FrisquetButton::dump_config() { LOG_BUTTON("", "Frisquet pairing button", this); }

}  // namespace frisquet_radio
}  // namespace esphome
#endif
