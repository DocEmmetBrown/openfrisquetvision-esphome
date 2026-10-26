#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// Writable selects. Writing needs `tx: write`; without it they only show what the boiler reports.
class FrisquetSelect : public select::Select, public Component {
 public:
  enum Kind : uint8_t { kHotWater, kZone1Mode };
  void set_parent(FrisquetRadio *parent) { parent_ = parent; }
  void set_kind(Kind kind) { kind_ = kind; }
  void dump_config() override;

 protected:
  void control(const std::string &value) override;
  FrisquetRadio *parent_{nullptr};
  Kind kind_{kHotWater};
};

}  // namespace frisquet_radio
}  // namespace esphome
#endif
