#pragma once

#include "esphome/core/defines.h"

#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#include "esphome/core/component.h"

#include "frisquet_radio.h"

namespace esphome {
namespace frisquet_radio {

// Zone 1 weekly programme as 84 hexadecimal characters (42 bytes, Sunday first, 6 bytes per day, bit set = comfort,
// bit 0 of the first byte = 00:00-00:30). Writing needs `tx: write` and a known zone block.
class ZoneProgramText : public text::Text, public Component {
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
