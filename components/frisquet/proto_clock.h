#pragma once
// Boiler clock: the date broadcast to the satellites (BCD, local time) drives the radio slots.
// The value is treated as UTC: only the minute/second phase inside the 10-minute cycle matters,
// and any whole-hour offset keeps that phase intact. Pure C++17.
#include <stdint.h>

namespace frisquet {

inline uint8_t bcd_to_decimal(uint8_t value) { return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F)); }

inline bool valid_bcd(uint8_t value) { return (value >> 4) <= 9 && (value & 0x0F) <= 9; }

// Days since 1970-01-01 for a proleptic Gregorian date.
inline int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline bool civil_to_timestamp(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second,
                               uint32_t &timestamp) {
  // Tentatively mirror the boiler clock's local-as-UTC representation; holiday timezone semantics are unverified.
  if (year < 2020 || year > 2105 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;
  static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const uint8_t max_day = month == 2 && leap ? 29 : month_days[month - 1];
  if (day < 1 || day > max_day) return false;
  const uint64_t value = static_cast<uint64_t>(days_from_civil(year, month, day)) * 86400u +
                         static_cast<uint32_t>(hour) * 3600u + static_cast<uint32_t>(minute) * 60u + second;
  if (value == 0 || value > UINT32_MAX) return false;
  timestamp = static_cast<uint32_t>(value);
  return true;
}

class BoilerClock {
 public:
  // BCD fields as broadcast: year is YY (20YY). Returns false when the date is implausible.
  bool set_from_bcd(uint32_t now_ms, uint8_t yy, uint8_t mo, uint8_t dd, uint8_t hh, uint8_t mi, uint8_t ss) {
    if (!valid_bcd(yy) || !valid_bcd(mo) || !valid_bcd(dd) || !valid_bcd(hh) || !valid_bcd(mi) || !valid_bcd(ss))
      return false;
    const uint32_t year = 2000u + bcd_to_decimal(yy);
    const uint32_t month = bcd_to_decimal(mo), day = bcd_to_decimal(dd);
    const uint32_t hour = bcd_to_decimal(hh), minute = bcd_to_decimal(mi), second = bcd_to_decimal(ss);
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
      return false;
    const int32_t days = days_from_civil(static_cast<int32_t>(year), month, day);
    base_seconds_ = static_cast<uint32_t>(days) * 86400u + hour * 3600u + minute * 60u + second;
    set_at_ms_ = now_ms;
    known_ = true;
    return true;
  }

  bool known() const { return known_; }

  // Seconds on the boiler clock, or seconds since start while it is unknown.
  uint32_t seconds(uint32_t now_ms) const {
    if (!known_) return now_ms / 1000u;
    return base_seconds_ + (now_ms - set_at_ms_) / 1000u;
  }

 private:
  bool known_ = false;
  uint32_t base_seconds_ = 0;
  uint32_t set_at_ms_ = 0;
};

}  // namespace frisquet
