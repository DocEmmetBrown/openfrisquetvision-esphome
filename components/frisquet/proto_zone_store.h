#pragma once
// Persistence format of the zone blocks, and the check that decides whether a restored block can still be trusted.
//
// The boiler cannot be asked for a zone block (reading 0xA154 is refused), so the Connect remembers the last one it was
// told. After a reboot that copy is only "restored": it may be stale if the satellite changed while we were away.
// It becomes "confirmed" by a relay from the boiler, or by the satellite's own 10-minute broadcast agreeing with it.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_decode.h"
#include "proto_encode.h"

namespace frisquet {

constexpr size_t kZoneStoreSize = 4 + 1 + 1 + 3 * kZoneBlockSize + 4;  // magic, version, known mask, blocks, CRC32

inline uint32_t store_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline void pack_zone_store(uint8_t known_mask, const uint8_t blocks[3][kZoneBlockSize], uint8_t *out) {
  memcpy(out, "FZB1", 4);
  out[4] = 1;
  out[5] = known_mask & 0x07;
  for (int zone = 0; zone < 3; ++zone) {
    if (known_mask & (1 << zone)) memcpy(out + 6 + zone * kZoneBlockSize, blocks[zone], kZoneBlockSize);
    else memset(out + 6 + zone * kZoneBlockSize, 0, kZoneBlockSize);
  }
  const uint32_t crc = store_crc32(out, kZoneStoreSize - 4);
  for (int i = 0; i < 4; ++i) out[kZoneStoreSize - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

// False for anything that is not a record written by pack_zone_store(): wrong size, magic, version or checksum.
inline bool unpack_zone_store(const uint8_t *in, size_t length, uint8_t &known_mask,
                              uint8_t blocks[3][kZoneBlockSize]) {
  if (!in || length != kZoneStoreSize || memcmp(in, "FZB1", 4) != 0 || in[4] != 1 || (in[5] & ~0x07) != 0) return false;
  uint32_t crc = 0;
  for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(in[kZoneStoreSize - 4 + i]) << (8 * i);
  if (crc != store_crc32(in, kZoneStoreSize - 4)) return false;
  known_mask = in[5];
  for (int zone = 0; zone < 3; ++zone) memcpy(blocks[zone], in + 6 + zone * kZoneBlockSize, kZoneBlockSize);
  return true;
}

enum class BlockCheck : uint8_t { kConsistent, kInconsistent, kInconclusive };

inline uint8_t bcd_to_bin(uint8_t value) { return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F)); }

// Does a satellite broadcast (mode byte, active setpoint, boiler clock) agree with `block`? Only a *consistent* answer
// proves anything, and only partially: a change to the programme or to an inactive setpoint can go unnoticed.
// `hour` and `minute` are the broadcast's BCD bytes, `weekday` 0 = Sunday.
inline BlockCheck check_block_against_broadcast(const uint8_t *block, uint8_t mode_raw, float setpoint,
                                                uint8_t weekday, uint8_t hour_bcd, uint8_t minute_bcd) {
  const DecodedZoneMode decoded = decode_zone_mode(mode_raw);
  if (decoded.mode == ZoneMode::kUnknown || isnan(setpoint) || weekday > 6) return BlockCheck::kInconclusive;
  const uint8_t mode = block[3];
  const bool auto_family = decoded.connect_compat && decoded.mode != ZoneMode::kFrostProtection;
  if (mode == 0x05 && !auto_family) return BlockCheck::kInconsistent;

  const bool comfort = decoded.mode == ZoneMode::kComfortPermanent || decoded.mode == ZoneMode::kComfortOverride ||
                       decoded.mode == ZoneMode::kComfortAuto;
  const bool frost = decoded.mode == ZoneMode::kFrostProtection;
  if (mode == 0x06 && !comfort) return BlockCheck::kInconsistent;
  if (mode == 0x07 && (comfort || frost)) return BlockCheck::kInconsistent;
  if (mode == 0x08 && !frost) return BlockCheck::kInconsistent;

  const uint8_t field = frost ? 2 : (comfort ? 0 : 1);
  if (fabsf(setpoint - temperature8(block[field])) > 0.06f) return BlockCheck::kInconsistent;

  // In Auto without a derogation, the programme decides between comfort and reduced.
  const bool derogation = decoded.mode == ZoneMode::kComfortOverride || decoded.mode == ZoneMode::kReducedOverride;
  if (mode == 0x05 && !frost && !derogation) {
    const uint8_t hour = bcd_to_bin(hour_bcd), minute = bcd_to_bin(minute_bcd);
    if (hour > 23 || minute > 59) return BlockCheck::kInconclusive;
    // Too close to a half-hour boundary the satellite may not have switched yet.
    if (minute % 30 < 2 || minute % 30 > 28) return BlockCheck::kInconclusive;
    const int slot = hour * 2 + minute / 30;
    const uint8_t *program = block + 6;
    const bool scheduled = (program[weekday * 6 + slot / 8] >> (slot % 8)) & 1;
    if (scheduled != comfort) return BlockCheck::kInconsistent;
  }
  return BlockCheck::kConsistent;
}

}  // namespace frisquet
