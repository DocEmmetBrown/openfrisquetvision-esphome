#pragma once
// Pairing on the broadcast network: NetworkID recovery (listen only) and association (listen, then confirm).
//
// While the boiler is in pairing mode it sends, on the broadcast sync word FF:FF:FF:FF, an 11-byte offer:
//   <dest> 80 <association> <id> <reception> 41 04 <NetworkID:4>
// A device that wants to pair answers with the reply header and the NetworkID, without a length byte (10 bytes):
//   80 <self> <association> <id> <reception|0x80> 41 <NetworkID:4>
// Format taken from OpenFrisquetVisio (FrisquetDevice::associer, FrisquetManager::recupererNetworkID); not yet
// captured on this installation.
//
// The session decides which sync word the radio must use at every moment: the broadcast one only while it listens
// or confirms, the installation's one otherwise. Every way out (success, timeout, cancel, emergency stop) therefore
// leaves the radio on a real network, which is what the MQTT firmware failed to do before b1e6934. Pure C++17.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_codec.h"
#include "proto_zone_store.h"

namespace frisquet {

constexpr uint8_t kBroadcastNetwork[4] = {0xFF, 0xFF, 0xFF, 0xFF};
constexpr size_t kAssociationOfferLength = 11;
constexpr size_t kAssociationConfirmLength = 10;
constexpr uint32_t kPairingListenMs = 30000;
constexpr uint8_t kAssociationConfirmRepeats = 10;
constexpr uint32_t kAssociationConfirmSpacingMs = 30;

// A NetworkID the radio may live on: neither the broadcast one nor all zeros.
inline bool is_usable_network(const uint8_t *network) {
  static const uint8_t kZero[4] = {0, 0, 0, 0};
  return network && memcmp(network, kBroadcastNetwork, 4) != 0 && memcmp(network, kZero, 4) != 0;
}

struct AssociationOffer {
  uint8_t destination = 0;
  uint8_t association = 0xFF;
  uint8_t message_id = 0;
  uint8_t reception = 0;
  uint8_t network[4] = {0, 0, 0, 0};
};

inline bool decode_association_offer(const uint8_t *frame, size_t length, AssociationOffer &out) {
  Header h;
  if (!frame || length != kAssociationOfferLength || !parse_header(frame, length, h)) return false;
  if (h.source != kIdBoiler || h.type != kAssociation || h.is_ack() || frame[6] != 4) return false;
  if (!is_usable_network(frame + 7) || h.association == 0xFF) return false;
  out.destination = h.destination;
  out.association = h.association;
  out.message_id = h.message_id;
  out.reception = h.reception;
  memcpy(out.network, frame + 7, 4);
  return true;
}

// Returns 0 when `offer` is not an association offer. The 4-byte payload is the offered NetworkID echoed back (as
// OpenFrisquetVisio does; works) or, when given, a version: a real Connect answers 01 21 01 02 (v21.01.02,
// EXTERNAL-RESEARCH 3.5), which the boiler may record at 7A30.
inline size_t build_association_confirm(const uint8_t *offer, size_t offer_length, uint8_t self_id, uint8_t *out,
                                        size_t capacity, const uint8_t *payload = nullptr) {
  AssociationOffer o;
  if (!out || capacity < kAssociationConfirmLength || !decode_association_offer(offer, offer_length, o)) return 0;
  out[0] = kIdBoiler;
  out[1] = self_id;
  out[2] = o.association;
  out[3] = o.message_id;
  out[4] = static_cast<uint8_t>(o.reception | 0x80);
  out[5] = kAssociation;
  memcpy(out + 6, payload ? payload : o.network, 4);
  return kAssociationConfirmLength;
}

enum class PairingKind : uint8_t { kRecoverNetwork, kAssociate };
enum class PairingState : uint8_t { kIdle, kListening, kConfirming, kSucceeded, kFailed, kCancelled };

inline const char *pairing_state_name(PairingState s) {
  switch (s) {
    case PairingState::kIdle: return "idle";
    case PairingState::kListening: return "listening";
    case PairingState::kConfirming: return "confirming";
    case PairingState::kSucceeded: return "succeeded";
    case PairingState::kFailed: return "failed";
    default: return "cancelled";
  }
}

class PairingSession {
 public:
  // False (nothing changes) when a session is already running. `current` may be unusable (a radio not paired yet):
  // a failed or cancelled session returns to it.
  bool start(PairingKind kind, uint8_t self_id, const uint8_t *current, uint32_t now_ms,
             uint32_t listen_ms = kPairingListenMs) {
    if (active() || current == nullptr) return false;
    kind_ = kind;
    self_id_ = self_id;
    memcpy(committed_, current, 4);
    memcpy(previous_, current, 4);
    association_ = 0xFF;
    offer_length_ = 0;
    confirms_sent_ = 0;
    deadline_ms_ = now_ms + listen_ms;
    state_ = PairingState::kListening;
    return true;
  }

  // The confirmation payload: a version (4 bytes), or nullptr to echo the NetworkID.
  void set_confirm_payload(const uint8_t *payload) {
    has_payload_ = payload != nullptr;
    if (payload) memcpy(payload_, payload, 4);
  }

  // Back to the network the session started from. No-op when idle or finished.
  void cancel() {
    if (!active()) return;
    memcpy(committed_, previous_, 4);
    state_ = PairingState::kCancelled;
  }

  // Every frame received while the session runs. True when it was the offer the session was waiting for.
  bool on_frame(uint32_t now_ms, const uint8_t *frame, size_t length) {
    if (state_ != PairingState::kListening) return false;
    AssociationOffer offer;
    if (!decode_association_offer(frame, length, offer)) return false;
    memcpy(offer_, frame, length);
    offer_length_ = length;
    association_ = offer.association;
    memcpy(learned_, offer.network, 4);
    if (kind_ == PairingKind::kRecoverNetwork) {
      memcpy(committed_, learned_, 4);
      state_ = PairingState::kSucceeded;
    } else {
      state_ = PairingState::kConfirming;
      next_confirm_ms_ = now_ms;
      // Confirming takes ~0.4 s; a radio that cannot transmit must not hold the broadcast network for long.
      deadline_ms_ = now_ms + 5000;
    }
    return true;
  }

  // Timeouts, and the confirmation frames to transmit (one per call, spaced). The caller sends `out` as is.
  bool poll(uint32_t now_ms, uint8_t *out, size_t &length) {
    length = 0;
    if (!active()) return false;
    if (static_cast<int32_t>(now_ms - deadline_ms_) >= 0) {
      memcpy(committed_, previous_, 4);
      state_ = PairingState::kFailed;
      return false;
    }
    if (state_ != PairingState::kConfirming || static_cast<int32_t>(now_ms - next_confirm_ms_) < 0) return false;
    length = build_association_confirm(offer_, offer_length_, self_id_, out, kAssociationConfirmLength,
                                       has_payload_ ? payload_ : nullptr);
    if (length == 0) {
      cancel();
      return false;
    }
    next_confirm_ms_ = now_ms + kAssociationConfirmSpacingMs;
    if (++confirms_sent_ >= kAssociationConfirmRepeats) {
      memcpy(committed_, learned_, 4);
      state_ = PairingState::kSucceeded;
    }
    return true;
  }

  // Finished sessions return to idle once their result has been taken.
  void acknowledge() {
    if (!active()) state_ = PairingState::kIdle;
  }

  bool active() const { return state_ == PairingState::kListening || state_ == PairingState::kConfirming; }
  PairingState state() const { return state_; }
  PairingKind kind() const { return kind_; }
  // The sync word the radio must use now.
  const uint8_t *network() const { return active() ? kBroadcastNetwork : committed_; }
  // After kSucceeded: the association byte the boiler gave (association only) and the offer, for the tx gate.
  uint8_t association() const { return association_; }
  const uint8_t *offer() const { return offer_length_ ? offer_ : nullptr; }
  size_t offer_length() const { return offer_length_; }

 private:
  PairingState state_ = PairingState::kIdle;
  PairingKind kind_ = PairingKind::kRecoverNetwork;
  uint8_t self_id_ = kIdConnect;
  uint8_t previous_[4] = {0, 0, 0, 0};
  uint8_t committed_[4] = {0, 0, 0, 0};
  uint8_t learned_[4] = {0, 0, 0, 0};
  uint8_t payload_[4] = {};
  bool has_payload_ = false;
  uint8_t association_ = 0xFF;
  uint8_t offer_[kAssociationOfferLength] = {};
  size_t offer_length_ = 0;
  uint8_t confirms_sent_ = 0;
  uint32_t deadline_ms_ = 0;
  uint32_t next_confirm_ms_ = 0;
};

// Persisted network identity: what pairing learned, used instead of the YAML values after a reboot.
// association 0xFF = keep the YAML association (a NetworkID recovery does not change it).
constexpr size_t kNetworkStoreSize = 4 + 1 + 4 + 1 + 4;  // magic, version, NetworkID, association, CRC32

inline void pack_network_store(const uint8_t *network, uint8_t association, uint8_t *out) {
  memcpy(out, "FNW1", 4);
  out[4] = 1;
  memcpy(out + 5, network, 4);
  out[9] = association;
  const uint32_t crc = store_crc32(out, kNetworkStoreSize - 4);
  for (int i = 0; i < 4; ++i) out[kNetworkStoreSize - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

inline bool unpack_network_store(const uint8_t *in, size_t length, uint8_t *network, uint8_t &association) {
  if (!in || length != kNetworkStoreSize || memcmp(in, "FNW1", 4) != 0 || in[4] != 1) return false;
  uint32_t crc = 0;
  for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(in[kNetworkStoreSize - 4 + i]) << (8 * i);
  if (crc != store_crc32(in, kNetworkStoreSize - 4) || !is_usable_network(in + 5)) return false;
  memcpy(network, in + 5, 4);
  association = in[9];
  return true;
}

}  // namespace frisquet
