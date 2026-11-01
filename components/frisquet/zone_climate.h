#pragma once

#include "esphome/core/defines.h"

#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// Zone 1 thermostat. Modes: AUTO (programme), HEAT (permanent Confort or Réduit, as presets), OFF (Hors gel: frost
// protection, the boiler is not switched off). Preset BOOST: comfort +1.0 C. The target temperature is the setpoint
// of the active mode. Everything shown comes from the confirmed zone block and the satellite's broadcast.
class FrisquetClimate : public climate::Climate, public Component {
 public:
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void dump_config() override;
  // Called by the radio whenever the zone may have changed: publishes only real changes.
  void refresh();

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  FrisquetRadio *parent_{nullptr};
  bool published_{false};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
