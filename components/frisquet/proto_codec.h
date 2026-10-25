#pragma once
// Frame header, addressing and primitive value codecs. Pure C++17, no ESPHome/Arduino.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace frisquet {

constexpr uint8_t kIdBoiler = 0x80;
constexpr uint8_t kIdZone1 = 0x08;
constexpr uint8_t kIdZone2 = 0x09;
constexpr uint8_t kIdZone3 = 0x0A;
constexpr uint8_t kIdOutdoorProbe = 0x20;
constexpr uint8_t kIdConnect = 0x7E;

enum MessageType : uint8_t { kRead = 0x03, kInit = 0x17, kAssociation = 0x41 };

constexpr size_t kHeaderSize = 6;

struct Header {
  uint8_t destination = 0;
  uint8_t source = 0;
  uint8_t association = 0;
  uint8_t message_id = 0;
  // 0x01: direct message; otherwise the final recipient (e.g. a zone satellite relayed
  // through the boiler). +0x80 marks an acknowledgement.
  uint8_t reception = 0;
  uint8_t type = 0;

  bool is_ack() const { return reception >= 0x80; }
};

inline bool parse_header(const uint8_t *data, size_t length, Header &out) {
  if (!data || length < kHeaderSize) return false;
  out.destination = data[0];
  out.source = data[1];
  out.association = data[2];
  out.message_id = data[3];
  out.reception = data[4];
  out.type = data[5];
  return true;
}

inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline int16_t be16s(const uint8_t *p) { return static_cast<int16_t>(be16(p)); }

// Temperatures are signed big-endian tenths of a degree. The boiler reports 127.0 and 129.0
// when a sensor is absent; both are mapped to NaN.
constexpr float kAbsentTemperatureA = 127.0f;
constexpr float kAbsentTemperatureB = 129.0f;

inline float temperature16(const uint8_t *p) { return be16s(p) / 10.0f; }

inline float temperature16_or_nan(const uint8_t *p) {
  const float value = temperature16(p);
  if (value == kAbsentTemperatureA || value == kAbsentTemperatureB) return NAN;
  return value;
}

// Pressure is signed big-endian in units of 1/5120 bar.
inline float pressure16(const uint8_t *p) { return be16s(p) / 5120.0f; }

}  // namespace frisquet
