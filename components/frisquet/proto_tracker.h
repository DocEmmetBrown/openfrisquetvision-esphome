#pragma once
// Pairs read requests with their answers when only listening. A read answer does not carry
// the address it answers: it is identified by (requester, message id) from the request seen
// earlier on the air. Pure C++17.
#include <stdint.h>

#include "proto_burner.h"
#include "proto_codec.h"

namespace frisquet {

// Memory addresses of the read requests the Connect issues. A second boiler family answers
// the same data at +0xC8.
constexpr uint16_t kAddrInfo = 0x79F2;           // temperatures, pressure, operating state
constexpr uint16_t kAddrConsumption = 0x7A18;    // daily consumption
constexpr uint16_t kAddrAnnualConsumption = 0xA063;
constexpr uint16_t kAddrHotWaterMode = 0xA0FC;
constexpr uint16_t kAddrDate = 0xA02B;
constexpr uint16_t kAddrStatus = 0xA029;         // what the satellite reads: outdoor, date, boiler state, zones
constexpr uint16_t kAddrFamilyOffset = 0x00C8;

enum class ReadKind : uint8_t { kUnknown, kInfo, kConsumption, kAnnualConsumption, kHotWaterMode, kDate, kStatus, kExplore, kBody, kFlue, kSatelliteConsigne };

inline ReadKind classify_read_address(uint16_t address) {
  switch (address) {
    case kAddrInfo: return ReadKind::kInfo;
    case kAddrConsumption: return ReadKind::kConsumption;
    case kAddrAnnualConsumption: return ReadKind::kAnnualConsumption;
    case kAddrHotWaterMode: return ReadKind::kHotWaterMode;
    case kAddrDate: return ReadKind::kDate;
    case kAddrStatus: return ReadKind::kStatus;
    case kAddrBodyLive: return ReadKind::kBody;
    case kAddrFlueSnapshot: return ReadKind::kFlue;
    default: break;
  }
  if (address >= kAddrFamilyOffset) return classify_read_address(static_cast<uint16_t>(address - kAddrFamilyOffset));
  return ReadKind::kUnknown;
}

class ReadTracker {
 public:
  static constexpr size_t kSlots = 8;
  static constexpr uint32_t kTimeoutMs = 30000;

  // Feed every validated frame. Returns true and sets `kind` when `frame` is the answer to a
  // tracked read request.
  bool on_frame(const uint8_t *frame, size_t length, uint32_t now_ms, ReadKind &kind) {
    Header header;
    if (!parse_header(frame, length, header) || header.type != kRead) return false;
    if (!header.is_ack()) {
      // 80 <requester> <assoc> <id> 01 03 <addr:2> <size:2>
      if (header.destination != kIdBoiler || length < kHeaderSize + 4) return false;
      remember_(header.source, header.message_id, be16(frame + kHeaderSize), now_ms);
      return false;
    }
    if (header.source != kIdBoiler) return false;
    for (Slot &slot : slots_) {
      if (!slot.used || slot.requester != header.destination || slot.message_id != header.message_id) continue;
      slot.used = false;
      if (now_ms - slot.at_ms > kTimeoutMs) return false;
      kind = classify_read_address(slot.address);
      return kind != ReadKind::kUnknown;
    }
    return false;
  }

 private:
  struct Slot {
    bool used = false;
    uint8_t requester = 0;
    uint8_t message_id = 0;
    uint16_t address = 0;
    uint32_t at_ms = 0;
  };

  void remember_(uint8_t requester, uint8_t message_id, uint16_t address, uint32_t now_ms) {
    Slot *target = &slots_[0];
    for (Slot &slot : slots_) {
      // Retransmissions of the same request refresh it instead of filling the table.
      if (slot.used && slot.requester == requester && slot.message_id == message_id) {
        target = &slot;
        break;
      }
      if (!slot.used) target = &slot;
      else if (target->used && slot.at_ms < target->at_ms) target = &slot;
    }
    target->used = true;
    target->requester = requester;
    target->message_id = message_id;
    target->address = address;
    target->at_ms = now_ms;
  }

  Slot slots_[kSlots];
};

}  // namespace frisquet
