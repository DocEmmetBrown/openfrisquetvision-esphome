#pragma once
// Decoders for the boiler's answers and broadcasts. Pure functions over validated payloads
// (CRC already checked, sync word removed). Field meanings come from the OpenFrisquetVisio
// reverse engineering; anything not yet confirmed on real traffic is marked "unverified".
#include <math.h>

#include "proto_codec.h"

namespace frisquet {

// ----- Read response for memory 0x79F2 (temperatures, pressure, operating state) -----
//
// Request : 80 7E <assoc> <id> 01 03 79 F2 00 1C
// Response: 7E 80 <assoc> <id> 81 03 38 <56 data bytes> = 63 bytes
struct BoilerInfo {
  float ambient[3] = {NAN, NAN, NAN};
  float flow_setpoint[3] = {NAN, NAN, NAN};   // "consigne départ"
  float setpoint[3] = {NAN, NAN, NAN};
  float outdoor = NAN;
  float hot_water = NAN;                       // ECS
  float heating_body = NAN;                    // corps de chauffe (CDC)
  float flow[3] = {NAN, NAN, NAN};             // départ
  float safety_probe = NAN;
  float flue = NAN;                            // reported as half of the raw reading
  float hot_water_power = NAN;                 // 0 on the observed boiler
  float heating_power = NAN;                   // 0 on the observed boiler
  float pressure = NAN;                        // bar
  uint16_t operating_mode_raw = 0xFFFF;
};

constexpr size_t kBoilerInfoMinLength = 7 + 44;

inline bool decode_boiler_info(const uint8_t *frame, size_t length, BoilerInfo &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length < kBoilerInfoMinLength) return false;
  const uint8_t *d = frame + 7;
  for (int z = 0; z < 3; ++z) {
    out.ambient[z] = temperature16_or_nan(d + 0 + 2 * z);
    out.flow_setpoint[z] = temperature16_or_nan(d + 6 + 2 * z);
    out.setpoint[z] = temperature16_or_nan(d + 12 + 2 * z);
    out.flow[z] = temperature16_or_nan(d + 24 + 2 * z);
  }
  out.outdoor = temperature16_or_nan(d + 18);
  out.hot_water = temperature16_or_nan(d + 20);
  out.heating_body = temperature16_or_nan(d + 22);
  out.safety_probe = temperature16_or_nan(d + 30);
  out.flue = temperature16(d + 32) / 2.0f;
  out.hot_water_power = temperature16(d + 34);
  out.heating_power = temperature16(d + 36);
  out.pressure = pressure16(d + 40);
  out.operating_mode_raw = be16(d + 42);
  return true;
}

enum class OperatingMode : uint8_t { kUnknown, kRadiators, kHotWater };

// Bit 0 must be set; bit 1 selects domestic hot water over radiators.
inline OperatingMode decode_operating_mode(uint16_t raw) {
  switch (raw & 0x0003) {
    case 0x0001: return OperatingMode::kRadiators;
    case 0x0003: return OperatingMode::kHotWater;
    default: return OperatingMode::kUnknown;
  }
}

// ----- Boiler state byte (broadcast to satellites) -----
struct BoilerState {
  bool fault = false;
  bool running = false;
  bool heating_stopped = false;
};

inline BoilerState decode_boiler_state(uint8_t raw) {
  BoilerState state;
  state.fault = (raw & 0x01) != 0;
  state.heating_stopped = (raw & 0x04) != 0;
  state.running = (raw & 0x08) != 0;
  return state;
}

enum class Activity : uint8_t { kUnknown, kStandby, kHeating, kHotWater };

// Same rule as the MQTT firmware: only the running flag and the radio operating mode count.
inline Activity boiler_activity(bool state_known, const BoilerState &state, OperatingMode mode) {
  if (state_known && !state.running) return Activity::kStandby;
  switch (mode) {
    case OperatingMode::kHotWater: return Activity::kHotWater;
    case OperatingMode::kRadiators:
      return state_known && state.heating_stopped ? Activity::kStandby : Activity::kHeating;
    default: return Activity::kUnknown;
  }
}

// ----- Satellite zone mode -----
enum class ZoneMode : uint8_t {
  kReducedPermanent = 0x00,
  kComfortPermanent = 0x01,
  kReducedOverride = 0x02,
  kComfortOverride = 0x03,
  kReducedAuto = 0x04,
  kComfortAuto = 0x05,
  kFrostProtection = 0x10,
  kUnknown = 0xFF,
};

constexpr uint8_t kConnectCompatXor = 0x20;

struct DecodedZoneMode {
  ZoneMode mode = ZoneMode::kUnknown;
  bool connect_compat = false;  // the raw byte had bit 0x20 set
};

inline ZoneMode zone_mode_direct(uint8_t value) {
  switch (value) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x10:
      return static_cast<ZoneMode>(value);
    default: return ZoneMode::kUnknown;
  }
}

// The broadcast byte is the zone's "options" byte as the satellite reports it. Verified on 2026-10-26,
// satellite left in Auto while the programme moved between comfort and reduced:
//   Auto, comfort  -> 0x25 or 0x21 (the 0x04 bit depends on what the Connect last wrote back)
//   Auto, reduced  -> 0x20
//   Reduced (manual, permanent) -> 0x00, with zone block mode 0x07
// So bit 0x20 marks an Auto-family value, bit 0x01 means comfort, and 0x00 alone is permanent reduced.
// Values without the 0x20 flag follow the older table (0x05 = auto comfort seen on 2026-10-25).
inline DecodedZoneMode decode_zone_mode(uint8_t raw) {
  DecodedZoneMode out;
  if (raw & 0xC0) return out;  // unknown high bits (e.g. a boost flag)
  if (raw & kConnectCompatXor) {
    out.connect_compat = true;
    const uint8_t low = raw & 0x1F;
    if (low & 0x10) out.mode = ZoneMode::kFrostProtection;
    else if (low & 0x02) out.mode = (low & 0x01) ? ZoneMode::kComfortOverride : ZoneMode::kReducedOverride;
    else out.mode = (low & 0x01) ? ZoneMode::kComfortAuto : ZoneMode::kReducedAuto;
    return out;
  }
  out.mode = zone_mode_direct(raw);
  return out;
}

// ----- Boiler → zone-1 satellite broadcast (type 0x17, 49 bytes, every 10 minutes) -----
//
// 08 80 FA <id> 81 17 2A <outdoor:2> <?:2> <date:6 YY MM DD hh mm ss> <state:1> <weekday:1>
//   <ambient Z1:2> <setpoint Z1:2> <?:1> <mode Z1:1> <?:4>
//   <ambient Z2:2> <setpoint Z2:2> <?:1> <mode Z2:1> <?:4>
//   <ambient Z3:2> <setpoint Z3:2> <?:1> <mode Z3:1> <?:4>
// The four bytes after each mode carry flow temperature and flow setpoint (unverified).
struct SatelliteBroadcast {
  float outdoor = NAN;
  uint8_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;  // year = YY
  uint8_t state_raw = 0;
  uint8_t weekday = 0;  // 0 = Sunday
  float ambient[3] = {NAN, NAN, NAN};
  float setpoint[3] = {NAN, NAN, NAN};
  uint8_t mode_raw[3] = {0xFF, 0xFF, 0xFF};
};

constexpr size_t kSatelliteBroadcastLength = 49;

inline bool decode_satellite_broadcast(const uint8_t *frame, size_t length, SatelliteBroadcast &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kInit || header.source != kIdBoiler || header.destination != kIdZone1) return false;
  if (length != kSatelliteBroadcastLength || frame[6] != 0x2A) return false;
  const uint8_t *d = frame + 7;
  out.outdoor = temperature16_or_nan(d + 0);
  out.year = d[4];
  out.month = d[5];
  out.day = d[6];
  out.hour = d[7];
  out.minute = d[8];
  out.second = d[9];
  out.state_raw = d[10];
  out.weekday = d[11];
  static const uint8_t kZoneOffset[3] = {12, 22, 32};
  for (int z = 0; z < 3; ++z) {
    const uint8_t *zone = d + kZoneOffset[z];
    out.ambient[z] = temperature16_or_nan(zone);
    out.setpoint[z] = temperature16_or_nan(zone + 2);
    out.mode_raw[z] = zone[5];
  }
  return true;
}

// ----- Zone configuration block (type 0x17, 63 bytes) -----
//
// The satellite writes its zone block to the boiler, which relays it to the Connect (and the
// Connect writes it back): <hdr:6> A1 54 <size:2> A1 54 <size:2> 30 <comfort:1> <reduced:1>
// <frost:1> <mode:1> <options:1> ... <weekly programme: 42 bytes>.
// Setpoint bytes are tenths of a degree above 5 degrees (0x8C = 19.0). `options` has the same
// bit layout as the satellite mode byte of the broadcast (0x01 comfort, 0x04 auto, 0x20 flag).
struct ZoneBlock {
  float comfort = NAN;
  float reduced = NAN;
  float frost = NAN;
  uint8_t mode = 0xFF;
  uint8_t options = 0xFF;
  uint8_t source = 0;
  uint8_t destination = 0;
};

constexpr size_t kZoneBlockLength = 63;

inline float temperature8(uint8_t raw) { return raw / 10.0f + 5.0f; }

inline bool decode_zone_block(const uint8_t *frame, size_t length, ZoneBlock &out) {
  Header header;
  if (!parse_header(frame, length, header) || header.type != kInit) return false;
  if (length != kZoneBlockLength) return false;
  if (frame[10] != 0xA1 || frame[11] != 0x54 || frame[12] != 0x00 || frame[13] != 0x18 || frame[14] != 0x30)
    return false;
  out.comfort = temperature8(frame[15]);
  out.reduced = temperature8(frame[16]);
  out.frost = temperature8(frame[17]);
  out.mode = frame[18];
  out.options = frame[19];
  out.source = header.source;
  out.destination = header.destination;
  return true;
}

// ----- Other read responses -----

// 0x7A18: daily consumption (kWh), two big-endian words after 18 bytes of unknown data.
struct Consumption {
  int16_t hot_water = -1;
  int16_t heating = -1;
};

inline bool decode_consumption(const uint8_t *frame, size_t length, Consumption &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length < 7 + 18 + 4) return false;
  out.hot_water = be16s(frame + 7 + 18);
  out.heating = be16s(frame + 7 + 20);
  return true;
}

// 0xA063: current-year monthly consumption in kWh, January to December, heating then hot water
// for each month (24 big-endian words, 55-byte answer). 0xA07B holds the previous year.
struct AnnualConsumption {
  int32_t heating = -1;
  int32_t hot_water = -1;
};

inline bool decode_annual_consumption(const uint8_t *frame, size_t length, AnnualConsumption &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length < 7 + 48 || frame[6] != 48) return false;
  int32_t heating = 0, hot_water = 0;
  for (int month = 0; month < 12; ++month) {
    heating += be16(frame + 7 + month * 4);
    hot_water += be16(frame + 7 + month * 4 + 2);
  }
  out.heating = heating;
  out.hot_water = hot_water;
  return true;
}

// 0xA0FC: hot-water mode, one byte. Bits 0x81 are frame bits to preserve when writing back.
enum class HotWaterMode : uint8_t {
  kMax = 0x00,
  kEco = 0x08,
  kEcoScheduled = 0x10,
  kEcoPlus = 0x18,
  kEcoPlusScheduled = 0x20,
  kStop = 0x28,
  kUnknown = 0xFF,
};

struct DecodedHotWaterMode {
  HotWaterMode mode = HotWaterMode::kUnknown;
  uint8_t frame_bits = 0;
};

inline bool decode_hot_water_mode(const uint8_t *frame, size_t length, DecodedHotWaterMode &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length < 7 + 2 || frame[6] != 0x02) return false;
  const uint8_t raw = frame[8];
  out.frame_bits = raw & 0x81;
  switch (raw & 0x7E) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
      out.mode = static_cast<HotWaterMode>(raw & 0x7E);
      break;
    default: out.mode = HotWaterMode::kUnknown;
  }
  return true;
}

}  // namespace frisquet
