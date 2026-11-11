#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

class FrisquetButton : public button::Button, public Component {
 public:
  enum Kind : uint8_t { kRecoverNetwork, kAssociate, kForgetNetwork, kHolidayActivate, kHolidayCancel, kOutdoorAssociate };
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void set_kind(Kind kind) { kind_ = kind; }
  void dump_config() override;

 protected:
  void press_action() override;
  FrisquetRadio *parent_{nullptr};
  Kind kind_{kRecoverNetwork};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
