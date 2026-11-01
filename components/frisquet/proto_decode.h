#pragma once
// Decoders for the boiler's answers and broadcasts. Pure functions over validated payloads
// (CRC already checked, sync word removed). Field meanings come from the OpenFrisquetVisio
// reverse engineering; anything not yet confirmed on real traffic is marked "unverified".
#include <math.h>
#include <stdio.h>

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
  bool fault = false;    // bit 0: anomaly with a radio partner (seen "anomalie avec Connect", DESIGN 10.3)
  bool anomaly = false;  // bit 7: boiler anomaly [EXTERNAL-RESEARCH, Freedom; not seen here]
  bool running = false;
  bool heating_stopped = false;
};

inline BoilerState decode_boiler_state(uint8_t raw) {
  BoilerState state;
  state.fault = (raw & 0x01) != 0;
  state.anomaly = (raw & 0x80) != 0;
  state.heating_stopped = (raw & 0x04) != 0;
  state.running = (raw & 0x08) != 0;
  return state;
}

// Burner state from the flue temperature of a 0x79F2 answer: 121.8 C while firing (29/10 11:17), 0 when stopped.
constexpr float kBurnerFlueThreshold = 40.0f;
inline bool burner_firing(float flue) { return !isnan(flue) && flue > kBurnerFlueThreshold; }

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
  // Bit 0x04 varies without meaning outside the 0x20 family too (verified 29/10/2026 by setting each mode from the
  // Connect): Confort 0x05, Réduit 0x00, Hors gel 0x14 or 0x10. Older notes read 0x05 as "auto, comfort": wrong.
  const uint8_t low = raw & static_cast<uint8_t>(~0x04);
  // Hors gel set on the satellite: 0x14 on 2026-10-28, 0x10 on 2026-10-29.
  if (low & 0x10) {
    out.mode = ZoneMode::kFrostProtection;
    return out;
  }
  out.mode = zone_mode_direct(low);
  return out;
}

// The zone block mode (05 Auto, 06 Confort, 07 Réduit, 08 Hors gel) a broadcast mode byte stands for; 0 when unknown.
// Derogations are Auto (05) with options 0x02: they get their own keys, 0x105 (comfort) and 0x205 (reduced).
constexpr uint16_t kSelectDerogationComfort = 0x105;
constexpr uint16_t kSelectDerogationReduced = 0x205;
inline uint16_t select_mode_from_broadcast(uint8_t raw) {
  const DecodedZoneMode decoded = decode_zone_mode(raw);
  switch (decoded.mode) {
    case ZoneMode::kFrostProtection: return 0x08;
    case ZoneMode::kComfortPermanent: return 0x06;
    case ZoneMode::kReducedPermanent: return 0x07;
    case ZoneMode::kComfortOverride: return kSelectDerogationComfort;
    case ZoneMode::kReducedOverride: return kSelectDerogationReduced;
    case ZoneMode::kUnknown: return 0;
    default: return 0x05;
  }
}

// Same key from a zone block: mode byte 3, options byte 4 (0x02 derogation, 0x01 comfort).
inline uint16_t select_mode_from_block(const uint8_t *block) {
  if (block[3] == 0x05 && (block[4] & 0x02))
    return (block[4] & 0x01) ? kSelectDerogationComfort : kSelectDerogationReduced;
  return block[3];
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

// The 0x2A data bytes after the length byte, shared by the broadcast and the 0xA029 read answer.
inline void decode_status_data(const uint8_t *d, SatelliteBroadcast &out) {
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
}

inline bool decode_satellite_broadcast(const uint8_t *frame, size_t length, SatelliteBroadcast &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kInit || header.source != kIdBoiler || header.destination != kIdZone1) return false;
  if (length != kSatelliteBroadcastLength || frame[6] != 0x2A) return false;
  decode_status_data(frame + 7, out);
  return true;
}

// Answer to our read of 0xA029 (0x15 words): the same data the satellite receives, at any time we ask.
inline bool decode_status_read(const uint8_t *frame, size_t length, SatelliteBroadcast &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return false;
  if (length != kSatelliteBroadcastLength || frame[6] != 0x2A) return false;
  decode_status_data(frame + 7, out);
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

// Weekly programme: 7 days (Sunday first) of 6 bytes, bit set = comfort, bit 0 of byte 0 = 00:00-00:30.
constexpr size_t kProgramSize = 42;

// "Dim 09:30-22:00 | Lun 08:00-21:30 | ..." (writes `capacity` bytes at most, always terminated).
inline size_t format_program_ranges(const uint8_t *program, char *out, size_t capacity) {
  static const char *const days[7] = {"Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam"};
  size_t n = 0;
  auto put = [&](const char *text) {
    while (*text && n + 1 < capacity) out[n++] = *text++;
  };
  for (int day = 0; day < 7; ++day) {
    if (day) put(" | ");
    put(days[day]);
    bool any = false;
    int start = -1;
    for (int slot = 0; slot <= 48; ++slot) {
      const bool on = slot < 48 && (program[day * 6 + slot / 8] >> (slot % 8)) & 1;
      if (on && start < 0) start = slot;
      if (!on && start >= 0) {
        char range[24];
        snprintf(range, sizeof(range), " %02d:%02d-%02d:%02d", start / 2, start % 2 * 30, slot / 2,
                 slot % 2 * 30);
        put(range);
        any = true;
        start = -1;
      }
    }
    if (!any) put(" -");
  }
  if (capacity) out[n < capacity ? n : capacity - 1] = '\0';
  return n;
}

// 84 hexadecimal characters <-> 42 bytes. Parsing is strict: wrong length or any non-hex character is refused.
inline bool parse_program_hex(const char *text, size_t length, uint8_t *out) {
  if (!text || length != kProgramSize * 2) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    uint8_t v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else return false;
    if (i % 2 == 0) out[i / 2] = v << 4;
    else out[i / 2] |= v;
  }
  return true;
}

inline void format_program_hex(const uint8_t *program, char *out) {
  static const char digits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < kProgramSize; ++i) {
    out[i * 2] = digits[program[i] >> 4];
    out[i * 2 + 1] = digits[program[i] & 0x0F];
  }
  out[kProgramSize * 2] = '\0';
}

// Diagnostics. Answer to a read of `words` words: 7E 80 <assoc> <id> 81 03 <2 * words> <data>.
inline const uint8_t *read_answer_data(const uint8_t *frame, size_t length, uint16_t words) {
  Header header;
  if (!parse_header(frame, length, header)) return nullptr;
  if (header.type != kRead || !header.is_ack() || header.source != kIdBoiler) return nullptr;
  if (frame[6] != 2 * words || length != 7u + 2u * words) return nullptr;
  return frame + 7;
}

struct PanelDiag {
  uint16_t version = 0;  // BCD: 0x1901 = V19.01
  uint8_t flow_min = 0;  // zone 1 flow limits, degrees (0x7946)
  uint8_t flow_max = 0;
};

// 0x7931 x 0x16: panel version at 0x7931, zone 1 flow min / max at 0x7946 (20 / 60 on our boiler).
inline bool decode_panel_diag(const uint8_t *frame, size_t length, PanelDiag &out) {
  const uint8_t *data = read_answer_data(frame, length, 0x16);
  if (!data) return false;
  out.version = be16(data);
  out.flow_min = data[2 * 0x15];
  out.flow_max = data[2 * 0x15 + 1];
  return out.flow_min <= out.flow_max;
}

// 0x7A30 x 4: two paired devices, each (index, id) then version (BCD, 0xFFFF = none recorded). Ours, 01/11/2026:
// 0308 2700 FF7E FFFF = satellite 08 V27.00, Connect 7E without a version.
inline bool format_paired_versions(const uint8_t *frame, size_t length, char *text, size_t capacity) {
  const uint8_t *data = read_answer_data(frame, length, 4);
  if (!data || !text || capacity < 64) return false;
  size_t used = 0;
  text[0] = 0;
  for (int i = 0; i < 2; ++i) {
    const uint8_t id = data[4 * i + 1];
    const uint16_t version = be16(data + 4 * i + 2);
    if (be16(data + 4 * i) == 0 || be16(data + 4 * i) == 0xFFFF) continue;
    const char *name = id == kIdConnect ? "Connect" : (id >= kIdZone1 && id <= kIdZone3) ? "Satellite" : "Appareil";
    char item[32];
    if (version == 0xFFFF || version == 0)
      snprintf(item, sizeof(item), "%s%s %02X sans version", used ? ", " : "", name, id);
    else
      snprintf(item, sizeof(item), "%s%s %02X V%X.%02X", used ? ", " : "", name, id, version >> 8, version & 0xFF);
    used += snprintf(text + used, capacity - used, "%s", item);
  }
  if (used == 0) snprintf(text, capacity, "aucun");
  return true;
}

// Text alert (type 0x43), sent by a device to the boiler [EXTERNAL-RESEARCH 3.3, captures]:
//   80 08 00 B4 01 43 00 1C "SATELLITE Z1\0CHANGER PILES \0"     low battery
//   ... 43 00 00                                                    no alert (at start-up, daily at 04:00)
// `text` gets the parts joined by " - ", empty for "no alert".
constexpr uint8_t kTextAlert = 0x43;

inline bool decode_text_alert(const uint8_t *frame, size_t length, uint8_t &source, char *text, size_t capacity) {
  Header header;
  if (!text || capacity < 2 || !parse_header(frame, length, header)) return false;
  if (header.type != kTextAlert || header.is_ack() || length < 8) return false;
  const size_t n = frame[7];
  if (length < 8 + n) return false;
  source = header.source;
  size_t used = 0;
  bool part_break = false, space = false;
  for (size_t i = 0; i < n && used + 4 < capacity; ++i) {
    const uint8_t c = frame[8 + i];
    if (c == 0) {
      part_break = true;
      continue;
    }
    if (c == ' ') {
      space = true;
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;
    if (used > 0 && part_break) {
      text[used++] = ' ';
      text[used++] = '-';
      text[used++] = ' ';
    } else if (used > 0 && space) {
      text[used++] = ' ';
    }
    part_break = space = false;
    text[used++] = static_cast<char>(c);
  }
  while (used > 0 && text[used - 1] == ' ') --used;
  text[used] = 0;
  return true;
}

// Event pushed by the boiler to the Connect (type 0x45, reception 05), acknowledged with 85 [EXTERNAL-RESEARCH 3.3]:
//   7E 80 <assoc> <id> 05 45 02 0A <device> <code> <Unix time, little-endian> ...
struct BoilerEvent {
  uint8_t device = 0;
  uint8_t code = 0;
  uint32_t time = 0;
};

inline bool decode_boiler_event(const uint8_t *frame, size_t length, BoilerEvent &out) {
  Header header;
  if (!parse_header(frame, length, header)) return false;
  if (header.type != 0x45 || header.source != kIdBoiler || header.reception != 0x05 || length < 14) return false;
  out.device = frame[8];
  out.code = frame[9];
  out.time = static_cast<uint32_t>(frame[10]) | static_cast<uint32_t>(frame[11]) << 8 |
             static_cast<uint32_t>(frame[12]) << 16 | static_cast<uint32_t>(frame[13]) << 24;
  return true;
}

}  // namespace frisquet
