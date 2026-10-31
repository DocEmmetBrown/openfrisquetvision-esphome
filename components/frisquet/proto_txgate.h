#pragma once
// Last line of defence before the radio: every frame the device wants to transmit is classified
// and checked against a policy. Anything not explicitly recognised is refused. Pure C++17.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_encode.h"
#include "proto_pairing.h"
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

enum class TxClass : uint8_t { kUnknown, kReadRequest, kZoneAck, kHotWaterWrite, kZoneWrite, kAssociationConfirm, kSatelliteConsigne };

// Which parts of a zone block a write may change; everything else must equal the last known block.
constexpr uint8_t kZoneFieldSetpoints = 0x01;  // comfort, reduced and frost temperatures
constexpr uint8_t kZoneFieldMode = 0x02;       // mode and options
constexpr uint8_t kZoneFieldProgram = 0x04;    // the 42-byte weekly programme

struct TxPolicy {
  TxLevel level = TxLevel::kNone;
  bool dry_run = true;      // log what would be sent, transmit nothing
  bool kill_switch = false; // forces listen-only
  uint8_t self_id = kIdConnect;
  uint8_t association = 0xFF;  // 0xFF = not associated: nothing may be sent
  uint8_t zone_write_mask = 0; // kZoneField* bits a zone write may change (0: zone writes refused)
  bool pairing = false;        // association confirmations allowed (YAML `pairing: true`)
  bool explore_reads = false;  // reads of addresses outside the whitelist allowed (YAML `explore: true`)
  bool live_body_reads = false;  // read the live heating-body temperature (0xA05E) every 15 s
  // Consigne exchanges in the name of the zone 1 satellite (YAML satellite_override + its association byte).
  bool satellite_override = false;
  uint8_t satellite_association = 0xFF;
};

constexpr uint16_t kMaxReadWords = 0x40;
// An answer holds at most 63 bytes: header, length byte and 0x1C words.
constexpr uint16_t kMaxExploreWords = 0x1C;

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
// acknowledgement must be exactly what build_zone_ack() derives from it. For an association
// confirmation it is the boiler's offer, and the frame must be exactly build_association_confirm().
inline TxVerdict evaluate_tx(const TxPolicy &policy, const uint8_t *frame, size_t length,
                             const uint8_t *relay = nullptr, size_t relay_length = 0,
                             TxClass *classified = nullptr, const uint8_t *zone_reference = nullptr) {
  TxClass cls = TxClass::kUnknown;
  if (classified) *classified = cls;
  if (policy.kill_switch) return TxVerdict::kBlockedKillSwitch;
  Header header;
  if (!frame || !parse_header(frame, length, header)) return TxVerdict::kMalformed;

  if (header.type == kAssociation) {
    if (length != kAssociationConfirmLength || !header.is_ack()) return TxVerdict::kMalformed;
    cls = TxClass::kAssociationConfirm;
    if (classified) *classified = cls;
    // The association byte is the one the offer gives, not ours: the exact match below checks it.
    uint8_t expected[kAssociationConfirmLength];
    if (header.source != policy.self_id || header.destination != kIdBoiler) return TxVerdict::kBlockedIdentity;
    if (!relay ||
        build_association_confirm(relay, relay_length, policy.self_id, expected, sizeof(expected)) !=
            kAssociationConfirmLength ||
        memcmp(expected, frame, kAssociationConfirmLength) != 0)
      return TxVerdict::kBlockedAddress;
    if (!policy.pairing) return TxVerdict::kBlockedLevel;
    return policy.dry_run ? TxVerdict::kDryRun : TxVerdict::kSend;
  }
  if (header.type == kInit && !header.is_ack() && length == kSatelliteConsigneLength && header.source >= kIdZone1 &&
      header.source <= kIdZone3) {
    cls = TxClass::kSatelliteConsigne;
    if (classified) *classified = cls;
    if (header.destination != kIdBoiler || header.reception != 0x01 || header.source != kIdZone1 ||
        policy.satellite_association == 0xFF || header.association != policy.satellite_association)
      return TxVerdict::kBlockedIdentity;
    const uint16_t address = satellite_consigne_address(header.source);
    const uint8_t body[9] = {0xA0, 0x29, 0x00, 0x15, static_cast<uint8_t>(address >> 8),
                             static_cast<uint8_t>(address & 0xFF), 0x00, 0x04, 0x08};
    if (memcmp(frame + 6, body, sizeof(body)) != 0) return TxVerdict::kBlockedAddress;
    const uint8_t *data = frame + 15;
    const int16_t ambient = static_cast<int16_t>(be16(data));
    const int16_t setpoint = static_cast<int16_t>(be16(data + 2));
    // Plausible ambient 0..40 C, setpoint 5..30 C in 0.5 steps, byte 4 zero.
    if (ambient < 0 || ambient > 400 || setpoint < 50 || setpoint > 300 || setpoint % 5 != 0 || data[4] != 0)
      return TxVerdict::kBlockedAddress;
    if (!policy.satellite_override) return TxVerdict::kBlockedLevel;
    return policy.dry_run ? TxVerdict::kDryRun : TxVerdict::kSend;
  }
  if (header.type == kRead && !header.is_ack()) {
    if (length != kReadRequestLength || header.reception != 0x01) return TxVerdict::kMalformed;
    cls = TxClass::kReadRequest;
  } else if (header.type == kInit && header.is_ack()) {
    if (length != kZoneAckLength || frame[6] != 0x30) return TxVerdict::kMalformed;
    cls = TxClass::kZoneAck;
  } else if (header.type == kInit && length == kZoneWriteLength) {
    if (header.reception < kIdZone1 || header.reception > kIdZone3) return TxVerdict::kMalformed;
    cls = TxClass::kZoneWrite;
  } else if (header.type == kInit) {
    if (length != kHotWaterModeWriteLength || header.reception != 0x01) return TxVerdict::kMalformed;
    cls = TxClass::kHotWaterWrite;
  } else {
    return TxVerdict::kBlockedUnknownFrame;
  }
  if (classified) *classified = cls;

  if (header.source != policy.self_id || header.destination != kIdBoiler) return TxVerdict::kBlockedIdentity;
  if (policy.association == 0xFF || header.association != policy.association) return TxVerdict::kBlockedIdentity;

  if (cls == TxClass::kReadRequest) {
    const uint16_t address = be16(frame + 6);
    const uint16_t words = be16(frame + 8);
    if (words == 0 || words > kMaxReadWords) return TxVerdict::kBlockedAddress;
    if (classify_read_address(address) == ReadKind::kUnknown && !(policy.explore_reads && words <= kMaxExploreWords))
      return TxVerdict::kBlockedAddress;
    if (policy.level < TxLevel::kRead) return TxVerdict::kBlockedLevel;
  } else if (cls == TxClass::kZoneWrite) {
    static const uint8_t kBody[9] = {0xA1, 0x54, 0x00, 0x18, 0xA1, 0x54, 0x00, 0x18, 0x30};
    if (memcmp(frame + 6, kBody, sizeof(kBody)) != 0) return TxVerdict::kBlockedAddress;
    const uint8_t *data = frame + 15;
    // Setpoints are 0.5 degree steps in 5.0..30.0: raw 0..250, multiples of 5, frost <= reduced <= comfort.
    for (int i = 0; i < 3; ++i)
      if (data[i] > 250 || data[i] % 5 != 0) return TxVerdict::kBlockedAddress;
    if (data[2] > data[1] || data[1] > data[0]) return TxVerdict::kBlockedAddress;
    // Mode byte: auto, comfort, reduced or frost only.
    if (data[3] < 0x05 || data[3] > 0x08) return TxVerdict::kBlockedAddress;
    // A write must start from the last known block: without it nothing is sent, and only the
    // fields the policy allows may differ from it.
    if (!zone_reference || policy.zone_write_mask == 0) return TxVerdict::kBlockedAddress;
    if (!(policy.zone_write_mask & kZoneFieldSetpoints) && memcmp(data, zone_reference, 3) != 0)
      return TxVerdict::kBlockedAddress;
    if (!(policy.zone_write_mask & kZoneFieldMode) && memcmp(data + 3, zone_reference + 3, 3) != 0)
      return TxVerdict::kBlockedAddress;
    // The unknown byte is never ours to change.
    if (data[5] != zone_reference[5]) return TxVerdict::kBlockedAddress;
    if (memcmp(data + 3, zone_reference + 3, 2) != 0) {
      // A mode change: the options byte must agree with the mode, keeping the bits we do not own.
      const uint8_t options = data[4];
      if ((options & ~0x17) != (zone_reference[4] & ~0x17)) return TxVerdict::kBlockedAddress;
      const uint8_t owned = options & 0x17;
      const bool coherent = (data[3] == 0x05 && (owned & 0x14) == 0x04) || (data[3] == 0x06 && owned == 0x01) ||
                            (data[3] == 0x07 && owned == 0x00) || (data[3] == 0x08 && owned == 0x10);
      if (!coherent) return TxVerdict::kBlockedAddress;
    }
    if (!(policy.zone_write_mask & kZoneFieldProgram) && memcmp(data + 6, zone_reference + 6, 42) != 0)
      return TxVerdict::kBlockedAddress;
    if (policy.level < TxLevel::kWrite) return TxVerdict::kBlockedLevel;
  } else if (cls == TxClass::kHotWaterWrite) {
    // Exactly the write the MQTT firmware sends, with a known hot-water mode: nothing else.
    static const uint8_t kBody[10] = {0xA0, 0xFC, 0x00, 0x01, 0xA0, 0xFC, 0x00, 0x01, 0x02, 0x00};
    if (memcmp(frame + 6, kBody, sizeof(kBody)) != 0 || !is_valid_hot_water_mode(frame[16] & 0x7E))
      return TxVerdict::kBlockedAddress;
    if (policy.level < TxLevel::kWrite) return TxVerdict::kBlockedLevel;
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
