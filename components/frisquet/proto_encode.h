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

}  // namespace frisquet
