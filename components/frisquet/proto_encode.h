#pragma once
// Builders for the frames this device may transmit. They produce the payload only: the radio
// adds the length byte and the CRC. Pure C++17.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_codec.h"
#include "proto_decode.h"

namespace frisquet {

constexpr size_t kReadRequestLength = 10;
constexpr size_t kZoneAckLength = 55;

inline void write_header(uint8_t *out, const Header &h) {
  out[0] = h.destination;
  out[1] = h.source;
  out[2] = h.association;
  out[3] = h.message_id;
  out[4] = h.reception;
  out[5] = h.type;
}

// 80 <requester> <association> <id> 01 03 <address:2> <size in words:2>
inline size_t build_read_request(uint8_t *out, size_t capacity, uint8_t requester, uint8_t association,
                                 uint8_t message_id, uint16_t address, uint16_t size_words) {
  if (!out || capacity < kReadRequestLength) return 0;
  Header h;
  h.destination = kIdBoiler;
  h.source = requester;
  h.association = association;
  h.message_id = message_id;
  h.reception = 0x01;
  h.type = kRead;
  write_header(out, h);
  out[6] = address >> 8;
  out[7] = address & 0xFF;
  out[8] = size_words >> 8;
  out[9] = size_words & 0xFF;
  return kReadRequestLength;
}

// A relayed zone block: boiler -> Connect, reception 0x08..0x0A (zone 1..3), write address 0xA154.
inline bool is_zone_relay(const uint8_t *frame, size_t length, uint8_t self_id) {
  Header header;
  ZoneBlock block;
  if (!decode_zone_block(frame, length, block) || !parse_header(frame, length, header)) return false;
  return header.destination == self_id && header.source == kIdBoiler && header.reception >= kIdZone1 &&
         header.reception <= kIdZone3;
}

// Acknowledgement of a zone relay: an echo of the 48-byte block, from us to the boiler, with the
// same association and message id and the acknowledgement bit set in `reception`:
//   80 <self> <assoc> <id> 88 17 30 <48 bytes of the block>
// Returns 0 when `relay` is not a zone relay addressed to `self_id`.
inline size_t build_zone_ack(const uint8_t *relay, size_t relay_length, uint8_t self_id, uint8_t *out,
                             size_t capacity) {
  if (!out || capacity < kZoneAckLength || !is_zone_relay(relay, relay_length, self_id)) return 0;
  Header relay_header;
  parse_header(relay, relay_length, relay_header);
  Header ack = relay_header;
  ack.destination = relay_header.source;
  ack.source = relay_header.destination;
  ack.reception = relay_header.reception | 0x80;
  write_header(out, ack);
  out[6] = 0x30;
  memcpy(out + 7, relay + 15, 48);
  return kZoneAckLength;
}

// Zone configuration block write (type 0x17): the Connect's way to change a zone's setpoints, mode and weekly
// programme. The boiler relays it to the satellite and answers (reception 0x88 | zone) once the satellite accepted it,
// which can take minutes: the satellite listens only around its own slot.
//   80 <self> <assoc> <id> <zone 08..0A> 17 A1 54 00 18 A1 54 00 18 30 <48-byte block>
constexpr size_t kZoneBlockSize = 48;
constexpr size_t kZoneWriteLength = 63;

// Setpoints are tenths of a degree above 5 degrees, in steps of 0.5: 19.0 -> 0x8C.
inline uint8_t encode_temperature8(float celsius) {
  if (celsius < 5.0f) celsius = 5.0f;
  if (celsius > 30.0f) celsius = 30.0f;
  return static_cast<uint8_t>((roundf(celsius * 2.0f) / 2.0f - 5.0f) * 10.0f + 0.5f);
}

inline size_t build_zone_write(uint8_t *out, size_t capacity, uint8_t requester, uint8_t association,
                               uint8_t message_id, uint8_t zone_id, const uint8_t *block) {
  if (!out || !block || capacity < kZoneWriteLength || zone_id < kIdZone1 || zone_id > kIdZone3) return 0;
  Header h;
  h.destination = kIdBoiler;
  h.source = requester;
  h.association = association;
  h.message_id = message_id;
  h.reception = zone_id;
  h.type = kInit;
  write_header(out, h);
  static const uint8_t kBody[9] = {0xA1, 0x54, 0x00, 0x18, 0xA1, 0x54, 0x00, 0x18, 0x30};
  memcpy(out + 6, kBody, sizeof(kBody));
  memcpy(out + 15, block, kZoneBlockSize);
  return kZoneWriteLength;
}

// 0xA0FC hot-water mode write (type 0x17, direct message): read and write of the same word, 2 data bytes.
//   80 <self> <assoc> <id> 01 17 A0 FC 00 01 A0 FC 00 01 02 00 <mode byte>
// The mode byte is the mode (bits 0x7E) plus the frame bits (0x81) last read from the boiler.
constexpr size_t kHotWaterModeWriteLength = 17;

inline bool is_valid_hot_water_mode(uint8_t mode_bits) {
  switch (mode_bits) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: return true;
    default: return false;
  }
}

inline uint8_t encode_hot_water_mode(HotWaterMode mode, uint8_t frame_bits) {
  return static_cast<uint8_t>((static_cast<uint8_t>(mode) & 0x7E) | (frame_bits & 0x81));
}

inline size_t build_hot_water_mode_write(uint8_t *out, size_t capacity, uint8_t requester, uint8_t association,
                                         uint8_t message_id, HotWaterMode mode, uint8_t frame_bits) {
  if (!out || capacity < kHotWaterModeWriteLength || !is_valid_hot_water_mode(static_cast<uint8_t>(mode))) return 0;
  Header h;
  h.destination = kIdBoiler;
  h.source = requester;
  h.association = association;
  h.message_id = message_id;
  h.reception = 0x01;
  h.type = kInit;
  write_header(out, h);
  static const uint8_t kBody[10] = {0xA0, 0xFC, 0x00, 0x01, 0xA0, 0xFC, 0x00, 0x01, 0x02, 0x00};
  memcpy(out + 6, kBody, sizeof(kBody));
  out[16] = encode_hot_water_mode(mode, frame_bits);
  return kHotWaterModeWriteLength;
}

// A satellite consigne exchange, sent in its name (source = zone id, its association): read A029 x15, write
// A02F + 5 * (zone - 1) x4 = ambient (2), active setpoint (2), 00, mode byte, last word (8 data bytes). The boiler
// answers at once with the 42-byte state page (08 80 <assoc> <id> 81 17 2A ...). Format from OpenFrisquetVisio.
constexpr size_t kSatelliteConsigneLength = 23;

inline uint16_t satellite_consigne_address(uint8_t zone_id) {
  return static_cast<uint16_t>(0xA02F + 5 * (zone_id - kIdZone1));
}

inline size_t build_satellite_consigne(uint8_t *out, size_t capacity, uint8_t zone_id, uint8_t association,
                                       uint8_t message_id, const uint8_t *data) {
  if (!out || !data || capacity < kSatelliteConsigneLength || zone_id < kIdZone1 || zone_id > kIdZone3) return 0;
  Header h;
  h.destination = kIdBoiler;
  h.source = zone_id;
  h.association = association;
  h.message_id = message_id;
  h.reception = 0x01;
  h.type = kInit;
  write_header(out, h);
  const uint16_t address = satellite_consigne_address(zone_id);
  const uint8_t body[9] = {0xA0, 0x29, 0x00, 0x15, static_cast<uint8_t>(address >> 8),
                           static_cast<uint8_t>(address & 0xFF), 0x00, 0x04, 0x08};
  memcpy(out + 6, body, sizeof(body));
  memcpy(out + 15, data, 8);
  return kSatelliteConsigneLength;
}

// Boiler pushes to the Connect when a setting is changed on its panel (seen 01/11/2026 12:42, ECS set by hand):
//   7E 80 <assoc> <id> 05 10 <addr:2> <words:2> <length> <data>     MEMORY_PUSH (seen: A0F0 x0D)
//   7E 80 <assoc> <id> 05 45 <21 bytes>                             BOILER_EVENT (frisquet-bridge, not seen here)
// Unacknowledged, the boiler repeats it 4 times one second apart. The official Connect answers (frisquet-bridge):
//   80 7E <assoc> <id> 85 10 <addr:2> <words:2>                     (10 bytes)
//   80 7E <assoc> <id> 85 45                                        (6 bytes)
constexpr uint8_t kMemoryPush = 0x10;
constexpr uint8_t kBoilerEvent = 0x45;

inline bool is_boiler_push(const uint8_t *frame, size_t length, uint8_t self_id) {
  Header h;
  if (!parse_header(frame, length, h)) return false;
  if (h.source != kIdBoiler || h.destination != self_id || h.reception != 0x05) return false;
  if (h.type == kMemoryPush) return length >= 11;
  return h.type == kBoilerEvent;
}

inline size_t build_push_ack(const uint8_t *push, size_t push_length, uint8_t self_id, uint8_t *out,
                             size_t capacity) {
  if (!out || capacity < 10 || !is_boiler_push(push, push_length, self_id)) return 0;
  Header h;
  parse_header(push, push_length, h);
  Header a;
  a.destination = kIdBoiler;
  a.source = self_id;
  a.association = h.association;
  a.message_id = h.message_id;
  a.reception = 0x85;
  a.type = h.type;
  write_header(out, a);
  if (h.type != kMemoryPush) return 6;
  memcpy(out + 6, push + 6, 4);
  return 10;
}

}  // namespace frisquet
