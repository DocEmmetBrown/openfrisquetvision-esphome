#pragma once
// Last line of defence before the radio: every frame the device wants to transmit is classified
// and checked against a policy. Anything not explicitly recognised is refused. Pure C++17.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_encode.h"
#include "proto_tracker.h"

namespace frisquet {

// Cumulative levels. `kWrite` is reserved: no zone/setpoint write is recognised yet, so such
// frames are refused at every level until their whitelist is added (phases 2b and 2c).
enum class TxLevel : uint8_t { kNone = 0, kRead = 1, kAck = 2, kWrite = 3 };

enum class TxVerdict : uint8_t {
  kSend,
  kDryRun,
  kBlockedKillSwitch,
  kBlockedLevel,
  kBlockedIdentity,
  kBlockedUnknownFrame,
  kBlockedAddress,
  kMalformed,
};

enum class TxClass : uint8_t { kUnknown, kReadRequest, kZoneAck };

struct TxPolicy {
  TxLevel level = TxLevel::kNone;
  bool dry_run = true;      // log what would be sent, transmit nothing
  bool kill_switch = false; // forces listen-only
  uint8_t self_id = kIdConnect;
  uint8_t association = 0xFF;  // 0xFF = not associated: nothing may be sent
};

constexpr uint16_t kMaxReadWords = 0x40;

inline const char *verdict_name(TxVerdict v) {
  switch (v) {
    case TxVerdict::kSend: return "SEND";
    case TxVerdict::kDryRun: return "DRY_RUN";
    case TxVerdict::kBlockedKillSwitch: return "BLOCKED_KILL_SWITCH";
    case TxVerdict::kBlockedLevel: return "BLOCKED_LEVEL";
    case TxVerdict::kBlockedIdentity: return "BLOCKED_IDENTITY";
    case TxVerdict::kBlockedUnknownFrame: return "BLOCKED_UNKNOWN_FRAME";
    case TxVerdict::kBlockedAddress: return "BLOCKED_ADDRESS";
    default: return "MALFORMED";
  }
}

// `relay` is the zone relay being acknowledged, when `frame` is an acknowledgement: the
// acknowledgement must be exactly what build_zone_ack() derives from it.
inline TxVerdict evaluate_tx(const TxPolicy &policy, const uint8_t *frame, size_t length,
                             const uint8_t *relay = nullptr, size_t relay_length = 0,
                             TxClass *classified = nullptr) {
  TxClass cls = TxClass::kUnknown;
  if (classified) *classified = cls;
  if (policy.kill_switch) return TxVerdict::kBlockedKillSwitch;
  Header header;
  if (!frame || !parse_header(frame, length, header)) return TxVerdict::kMalformed;

  if (header.type == kRead && !header.is_ack()) {
    if (length != kReadRequestLength || header.reception != 0x01) return TxVerdict::kMalformed;
    cls = TxClass::kReadRequest;
  } else if (header.type == kInit && header.is_ack()) {
    if (length != kZoneAckLength || frame[6] != 0x30) return TxVerdict::kMalformed;
    cls = TxClass::kZoneAck;
  } else {
    return TxVerdict::kBlockedUnknownFrame;
  }
  if (classified) *classified = cls;

  if (header.source != policy.self_id || header.destination != kIdBoiler) return TxVerdict::kBlockedIdentity;
  if (policy.association == 0xFF || header.association != policy.association) return TxVerdict::kBlockedIdentity;

  if (cls == TxClass::kReadRequest) {
    const uint16_t address = be16(frame + 6);
    const uint16_t words = be16(frame + 8);
    if (classify_read_address(address) == ReadKind::kUnknown || words == 0 || words > kMaxReadWords)
      return TxVerdict::kBlockedAddress;
    if (policy.level < TxLevel::kRead) return TxVerdict::kBlockedLevel;
  } else {
    uint8_t expected[kZoneAckLength];
    if (!relay ||
        build_zone_ack(relay, relay_length, policy.self_id, expected, sizeof(expected)) != kZoneAckLength ||
        memcmp(expected, frame, kZoneAckLength) != 0)
      return TxVerdict::kBlockedAddress;
    if (policy.level < TxLevel::kAck) return TxVerdict::kBlockedLevel;
  }
  return policy.dry_run ? TxVerdict::kDryRun : TxVerdict::kSend;
}

}  // namespace frisquet
