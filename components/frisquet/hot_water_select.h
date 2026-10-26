#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// Hot-water (ECS) mode. Writing it needs `tx: write`; without it the select only shows the mode.
class HotWaterSelect : public select::Select, public Component {
 public:
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void dump_config() override;

 protected:
  void control(const std::string &value) override;
  FrisquetRadio *parent_{nullptr};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
