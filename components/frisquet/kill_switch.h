#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// ON = emergency stop engaged: the radio never transmits. Engage it to return to listen-only.
class KillSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void write_state(bool state) override;
  FrisquetRadio *parent_{nullptr};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
