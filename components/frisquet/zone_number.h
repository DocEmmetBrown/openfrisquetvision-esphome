#pragma once

#include "esphome/core/defines.h"

#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// A zone 1 setpoint (comfort, reduced or frost). Writing it needs `tx: write`; the value shown is the one the
// boiler last relayed, so a change is visible only once the satellite has accepted it.
class ZoneSetpointNumber : public number::Number, public Component {
 public:
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void set_field(uint8_t field) { field_ = field; }
  void dump_config() override;

 protected:
  void control(float value) override;
  FrisquetRadio *parent_{nullptr};
  uint8_t field_{0};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
