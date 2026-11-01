#pragma once
// Slot scheduling for the periodic radio transactions. Pure C++17: callers pass the clock.
//
// Slots are derived from the boiler/NTP clock (or uptime before sync). Each slot gets at most
// one attempt, even on failure: there is no catch-up burst. Nothing is sent in the quiet window
// around the satellites' own slots (Z1 at :00, Z2 at :01, Z3 at :02, +/-30 s, every 10 minutes).
#include <stddef.h>
#include <stdint.h>

namespace frisquet {

constexpr uint32_t kSlotCycleSeconds = 600;
constexpr uint32_t kSlotWindowSeconds = 10;

inline bool in_quiet_window(uint32_t seconds) {
  const uint32_t phase = seconds % kSlotCycleSeconds;
  return phase < 150 || phase >= 570;
}

// Around the zone 1 satellite's own exchange (:x0:08, measured 08 +/- 2 s): no direct consigne there. A consigne is one
// 0.15 s exchange, so outside these 40 s it may go even in the quiet window (zones 2 and 3, at :x1 and :x2, do not exist on
// the reference installation: extend this window if they do).
inline bool near_zone1_exchange(uint32_t seconds) {
  const uint32_t phase = seconds % kSlotCycleSeconds;
  return phase < 28 || phase >= 588;
}

class SlotSchedule {
 public:
  // True once per slot: `period` and `offset` in seconds, `seconds` the current clock,
  // `uptime_ms` a monotonic millisecond counter (wrap-safe).
  bool due(uint32_t seconds, uint32_t uptime_ms, uint32_t period, uint32_t offset) {
    if (in_quiet_window(seconds) || period == 0 || offset >= period) return false;
    const uint32_t phase = seconds % period;
    if (phase < offset || phase - offset >= kSlotWindowSeconds) return false;
    // A second attempt inside the slot, or a duplicate after a clock correction, is refused.
    if (attempted_ && uptime_ms - last_attempt_ms_ < (period - kSlotWindowSeconds) * 1000UL) return false;
    attempted_ = true;
    last_attempt_ms_ = uptime_ms;
    return true;
  }

  // Forget past attempts, e.g. when the clock switches from uptime to the boiler's date.
  void reset() {
    attempted_ = false;
    last_attempt_ms_ = 0;
  }

 private:
  bool attempted_ = false;
  uint32_t last_attempt_ms_ = 0;
};

// Slots used by the active Connect (seconds into each 10-minute cycle, or into the hour).
// 0x79F2 at :03 and :08: the boiler refreshes it only then (zone values at both, its own temperatures and the flue
// at :08 only; measured on 2026-10-29, 41 changes in 158 reads made every minute).
constexpr uint32_t kTemperatureOffsets[] = {180, 480};
constexpr size_t kTemperatureSlots = sizeof(kTemperatureOffsets) / sizeof(kTemperatureOffsets[0]);
// 0xA029 (boiler state byte, clock) every minute from :03 to :09.
constexpr uint32_t kStatusOffsets[] = {180, 240, 300, 360, 420, 480, 540};
constexpr size_t kStatusSlots = sizeof(kStatusOffsets) / sizeof(kStatusOffsets[0]);
constexpr uint32_t kConsumptionPeriod = 3600;
constexpr uint32_t kConsumptionOffset = 300;                  // :05 of each hour
constexpr uint32_t kHotWaterModePeriod = 3600;
constexpr uint32_t kHotWaterModeOffset = 420;                 // :07 of each hour

}  // namespace frisquet
