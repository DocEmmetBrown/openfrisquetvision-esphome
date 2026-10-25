#pragma once
// Frame integrity: CRC16 and single-zero-run bit-slip repair.
//
// Derived from the receive path of OpenFrisquetVisio (RadioCrc.h), which has no
// published licence. Keep this repository private until that is clarified (D6).
//
// Raw capture layout (after the sync word): length byte, payload, CRC MSB, CRC LSB,
// then margin. The CRC covers the length byte too (SX1262 variable-length GFSK).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace frisquet {
namespace crc {

constexpr size_t kMaxPayload = 63;
constexpr size_t kRawLength = 68;
constexpr size_t kHeaderLength = 6;
constexpr size_t kMinZeroRun = 16;

enum class Status { Valid, Repaired, Invalid, Ambiguous, InvalidLength };

inline const char *status_name(Status status) {
  switch (status) {
    case Status::Valid: return "VALID";
    case Status::Repaired: return "REPAIRED";
    case Status::Ambiguous: return "AMBIGUOUS";
    case Status::InvalidLength: return "INVALID_LENGTH";
    default: return "INVALID";
  }
}

struct Result {
  Status status = Status::Invalid;
  size_t length = 0;
  size_t bit = 0;           // Position in the raw capture, MSB first.
  int8_t adjustment = 0;    // +1: a zero was inserted, -1: a zero was removed.
  uint8_t corrected_bits = 0;
};

inline uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0x1D0F;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc ^ 0xFFFF;
}

inline bool get_bit(const uint8_t *data, size_t bit) { return (data[bit / 8] >> (7 - bit % 8)) & 1; }

inline bool valid(const uint8_t *raw, size_t length) {
  const size_t offset = length + 1;
  const uint16_t received = (static_cast<uint16_t>(raw[offset]) << 8) | raw[offset + 1];
  return crc16(raw, offset) == received;
}

// Never modifies `raw`, nor `payload` when the frame is rejected. At most one zero is
// inserted or removed, inside one run of zeros after the header. All candidates are
// examined before accepting so that an ambiguous repair is reported as such.
inline Result decode(const uint8_t *raw, size_t raw_length, uint8_t *payload) {
  Result result;
  if (!raw || !payload || raw_length == 0 || raw_length > kRawLength) return result;
  result.length = raw[0];
  if (result.length < kHeaderLength || result.length > kMaxPayload) {
    result.status = Status::InvalidLength;
    result.length = 0;
    return result;
  }
  const size_t frame_length = result.length + 3;
  if (raw_length < frame_length) return result;
  if (valid(raw, result.length)) {
    memcpy(payload, raw + 1, result.length);
    result.status = Status::Valid;
    return result;
  }
  // One extra bit is needed to cancel a surplus zero.
  if (raw_length * 8 <= frame_length * 8) return result;

  uint8_t unique[kMaxPayload + 3];
  uint8_t candidate[kMaxPayload + 3];
  bool found = false;
  const size_t payload_end = (result.length + 1) * 8;
  for (size_t bit = (kHeaderLength + 1) * 8; bit < payload_end;) {
    if (get_bit(raw, bit)) {
      ++bit;
      continue;
    }
    const size_t start = bit;
    while (bit < payload_end && !get_bit(raw, bit)) ++bit;
    if (bit - start < kMinZeroRun) continue;
    // Do not start in the middle of a run that began in the header.
    if (!get_bit(raw, start - 1)) continue;
    for (int8_t adjustment = 1; adjustment >= -1; adjustment -= 2) {
      memset(candidate, 0, frame_length);
      for (size_t out = 0; out < frame_length * 8; ++out) {
        bool value;
        if (out < start) value = get_bit(raw, out);
        else if (adjustment == 1) value = out == start ? false : get_bit(raw, out - 1);
        else value = get_bit(raw, out + 1);
        if (value) candidate[out / 8] |= uint8_t(1U << (7 - out % 8));
      }
      if (!valid(candidate, result.length)) continue;
      if (found && memcmp(unique, candidate, frame_length) != 0) {
        result.status = Status::Ambiguous;
        return result;
      }
      if (!found) {
        memcpy(unique, candidate, frame_length);
        result.bit = start;
        result.adjustment = adjustment;
        result.corrected_bits = 1;
        found = true;
      }
    }
  }
  if (found) {
    memcpy(payload, unique + 1, result.length);
    result.status = Status::Repaired;
  }
  return result;
}

}  // namespace crc
}  // namespace frisquet
