#pragma once

#include <math.h>
#include <stdint.h>

namespace frisquet {

constexpr uint8_t kIdOutdoorSensor = 0x20;
constexpr uint32_t kOutdoorStaleMs = 3UL * 60UL * 1000UL;
constexpr uint32_t kOutdoorExpiryMs = 180UL * 60UL * 1000UL;

class OutdoorSample {
 public:
  bool update(float celsius, int32_t age_seconds, uint32_t now) {
    if (!isfinite(celsius) || celsius < -40 || celsius > 60 || age_seconds < 0 ||
        static_cast<uint32_t>(age_seconds) >= kOutdoorExpiryMs / 1000)
      return false;
    temperature_ = static_cast<int16_t>(lroundf(celsius * 10));
    received_ms_ = now;
    initial_age_ms_ = static_cast<uint32_t>(age_seconds) * 1000;
    known_ = true;
    expired_ = false;
    return true;
  }
  uint32_t age_ms(uint32_t now) const {
    const uint32_t elapsed = now - received_ms_;
    return elapsed >= kOutdoorExpiryMs || initial_age_ms_ >= kOutdoorExpiryMs - elapsed
               ? kOutdoorExpiryMs
               : initial_age_ms_ + elapsed;
  }
  bool usable(uint32_t now) const { return known_ && !expired_ && age_ms(now) < kOutdoorExpiryMs; }
  bool stale(uint32_t now) const { return !known_ || expired_ || age_ms(now) >= kOutdoorStaleMs; }
  void tick(uint32_t now) { if (known_ && age_ms(now) >= kOutdoorExpiryMs) expired_ = true; }
  int16_t temperature() const { return temperature_; }
  bool known() const { return known_; }

 private:
  bool known_{false};
  bool expired_{false};
  int16_t temperature_{0};
  uint32_t received_ms_{0};
  uint32_t initial_age_ms_{0};
};

}  // namespace frisquet
