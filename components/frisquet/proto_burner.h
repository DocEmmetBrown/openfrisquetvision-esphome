#pragma once
// Burner state from the live heating-body temperature at 0xA05E (tenths of a degree), read every 15 s.
//
// The boiler sends no frame when the burner starts or stops, and its 0x79F2 data is a 10-minute snapshot. 0xA05E is
// live: on 2026-10-31 a hot-water draw made it fall by ~1.5 C every 14 s, the flame (10:54:35 - 10:56:30) turned it
// into a rise of 2 to 3 C every 12 s (with ~30 s of thermal lag at both ends), then it fell again. Pure C++17.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "proto_codec.h"

namespace frisquet {

constexpr uint16_t kAddrBodyLive = 0xA05E;
constexpr uint32_t kBodyReadPeriodMs = 15000;

// Answer to a 1-word read of 0xA05E: 7E 80 <assoc> <id> 81 03 02 <hi> <lo>.
inline bool decode_body_live(const uint8_t *frame, size_t length, float &celsius) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length != 9 || frame[6] != 0x02) return false;
  celsius = temperature16(frame + 7);
  return true;
}

enum class BurnerState : uint8_t { kUnknown, kOff, kOn };

class BurnerDetector {
 public:
  // Rise per 15 s that turns the burner on, and the one at or below which it is off again. The sensor moves in steps
  // of ~0.5 C (+0.6 C seen at rest on 31/10 11:13); a firing burner adds 1.8 to 3.5 C per 15 s.
  static constexpr float kOnRise = 1.0f;
  static constexpr float kOffRise = 0.1f;
  // Samples further apart than this (the satellites' window) only set a new reference, except that a lower
  // temperature after the gap means the burner is off.
  static constexpr uint32_t kMaxGapMs = 45000;

  BurnerState feed(uint32_t now_ms, float celsius) {
    if (isnan(celsius)) return state_;
    if (has_last_ && now_ms - last_ms_ > 0 && now_ms - last_ms_ <= kMaxGapMs) {
      const float rise = (celsius - last_) * 15000.0f / static_cast<float>(now_ms - last_ms_);
      if (rise >= kOnRise) state_ = BurnerState::kOn;
      else if (rise <= kOffRise) state_ = BurnerState::kOff;
      else if (state_ == BurnerState::kUnknown) state_ = BurnerState::kOff;
    } else if (has_last_ && celsius < last_) {
      state_ = BurnerState::kOff;
    }
    has_last_ = true;
    last_ = celsius;
    last_ms_ = now_ms;
    return state_;
  }
  BurnerState state() const { return state_; }

 private:
  BurnerState state_ = BurnerState::kUnknown;
  bool has_last_ = false;
  float last_ = 0;
  uint32_t last_ms_ = 0;
};

}  // namespace frisquet
