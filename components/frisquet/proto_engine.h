#pragma once
// The Connect's behaviour as a pure state machine: acknowledge zone relays, run the scheduled
// reads one at a time, and report the answers. It never touches the radio: the caller feeds
// received frames and the clock, and transmits what poll() returns (after the gate's verdict).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_clock.h"
#include "proto_schedule.h"
#include "proto_transaction.h"
#include "proto_txgate.h"
#include "proto_zone_store.h"

namespace frisquet {

struct EngineStats {
  uint32_t acks_sent = 0;
  uint32_t acks_blocked = 0;
  uint32_t reads_started = 0;
  uint32_t reads_ok = 0;
  uint32_t reads_failed = 0;
  uint32_t dry_runs = 0;
  uint32_t blocked = 0;
  uint32_t writes_acked = 0;
  uint32_t writes_verified = 0;
  uint32_t writes_failed = 0;
  uint32_t zone_bursts = 0;
  uint32_t zone_writes_acked = 0;
  uint32_t zone_writes_failed = 0;
};

// Life of a hot-water mode write: queued, sent until the boiler acknowledges, then read back.
// kSuperseded: a zone write abandoned because the satellite relayed a block changed locally meanwhile (local wins).
enum class WriteState : uint8_t { kIdle, kQueued, kSending, kAcked, kVerified, kFailed, kNotSent, kSuperseded };

// Same zone settings, ignoring the option bits the satellite rewrites by itself (0x04, 0x20, others not owned).
inline bool same_zone_settings(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 4) == 0 && ((a[4] ^ b[4]) & 0x13) == 0 && memcmp(a + 5, b + 5, kZoneBlockSize - 5) == 0;
}

struct TxRequest {
  uint8_t frame[64] = {};
  size_t length = 0;
  TxVerdict verdict = TxVerdict::kBlockedLevel;
  TxClass kind = TxClass::kUnknown;
};

struct ReadResult {
  ReadKind kind = ReadKind::kUnknown;
  uint16_t address = 0;  // set for exploratory reads
  uint8_t frame[64] = {};
  size_t length = 0;
};

class ConnectEngine {
 public:
  void set_policy(const TxPolicy &policy) { policy_ = policy; }
  const TxPolicy &policy() const { return policy_; }
  void set_kill_switch(bool value) { policy_.kill_switch = value; }
  void set_next_message_id(uint8_t id) { next_id_ = id; }
  uint8_t next_message_id() const { return next_id_; }
  const EngineStats &stats() const { return stats_; }
  BoilerClock &clock() { return clock_; }

  // Queue the reads done once at start-up (the MQTT firmware does the same).
  void request_initial_reads() {
    enqueue_(ReadKind::kInfo);
    enqueue_(ReadKind::kConsumption);
    enqueue_(ReadKind::kAnnualConsumption);
    enqueue_(ReadKind::kHotWaterMode);
  }

  // Ask for a hot-water mode change. Refused while another write is under way, and until a read of
  // 0xA0FC has told us the frame bits to preserve (never guess them).
  bool request_hot_water_mode(HotWaterMode mode) {
    if (!is_valid_hot_water_mode(static_cast<uint8_t>(mode)) || !hot_water_known_) return false;
    if (write_state_ == WriteState::kQueued || write_state_ == WriteState::kSending ||
        write_state_ == WriteState::kAcked)
      return false;
    requested_mode_ = mode;
    write_state_ = WriteState::kQueued;
    return true;
  }
  WriteState write_state() const { return write_state_; }
  HotWaterMode requested_hot_water_mode() const { return requested_mode_; }
  HotWaterMode hot_water_mode() const { return hot_water_mode_; }
  bool hot_water_mode_known() const { return hot_water_known_; }

  // Zone blocks: learned from the relays the boiler sends us (the block cannot be read back).
  bool zone_block_known(uint8_t zone_index) const { return zone_index < 3 && zone_known_[zone_index]; }
  const uint8_t *zone_block(uint8_t zone_index) const { return zone_index < 3 ? zone_block_[zone_index] : nullptr; }
  // A block is "confirmed" once the boiler relayed it (or we wrote it and the boiler echoed it) since boot, or the
  // satellite's broadcast agreed with a restored copy. Writes start only from a confirmed block.
  bool zone_block_confirmed(uint8_t zone_index) const { return zone_index < 3 && zone_confirmed_[zone_index]; }

  // Blocks remembered across reboots: known (shown) but not confirmed (not writable).
  void restore_zone_block(uint8_t zone_index, const uint8_t *block) {
    if (zone_index >= 3 || !block || zone_known_[zone_index]) return;
    memcpy(zone_block_[zone_index], block, kZoneBlockSize);
    zone_known_[zone_index] = true;
    zone_confirmed_[zone_index] = false;
  }

  // The satellite's broadcast for a zone. Confirms a restored block that agrees with it, drops one that does not.
  void on_zone_broadcast(uint8_t zone_index, uint8_t mode_raw, float setpoint, uint8_t weekday, uint8_t hour_bcd,
                         uint8_t minute_bcd) {
    if (zone_index >= 3 || !zone_known_[zone_index] || zone_confirmed_[zone_index] || zone_write_pending()) return;
    switch (check_block_against_broadcast(zone_block_[zone_index], mode_raw, setpoint, weekday, hour_bcd, minute_bcd)) {
      case BlockCheck::kConsistent:
        zone_confirmed_[zone_index] = true;
        break;
      case BlockCheck::kInconsistent:
        zone_known_[zone_index] = false;
        zone_dirty_ = true;
        break;
      default: break;
    }
  }

  // True once after every change of a known block (relay, verified write, discarded copy): time to persist.
  bool take_zone_dirty() {
    const bool dirty = zone_dirty_;
    zone_dirty_ = false;
    return dirty;
  }
  uint8_t zone_known_mask() const {
    return static_cast<uint8_t>((zone_known_[0] ? 1 : 0) | (zone_known_[1] ? 2 : 0) | (zone_known_[2] ? 4 : 0));
  }
  const uint8_t (*zone_blocks() const)[kZoneBlockSize] { return zone_block_; }
  WriteState zone_write_state() const { return zone_state_; }
  uint8_t zone_write_zone() const { return zone_index_; }
  bool zone_write_pending() const { return zone_state_ == WriteState::kQueued || zone_state_ == WriteState::kSending; }

  // Ask for a new block for a zone. Refused while a zone write is pending and until the zone's current block
  // is known: every byte not meant to change is copied from it. The gate decides what may really change.
  bool request_zone_block(uint8_t zone_index, const uint8_t *block, uint8_t fields) {
    if (zone_index >= 3 || !zone_confirmed_[zone_index] || zone_write_pending() || !block) return false;
    memcpy(zone_request_, block, kZoneBlockSize);
    zone_fields_ = fields;
    zone_index_ = zone_index;
    zone_state_ = WriteState::kQueued;
    return true;
  }
  // Mode: 0x05 auto, 0x06 comfort, 0x07 reduced, 0x08 frost protection. The options byte follows the MQTT firmware's
  // Zone::setMode: only bits 0x17 change; in Auto the current comfort bit is kept (the satellite recomputes it).
  bool request_zone_mode(uint8_t zone_index, uint8_t mode) {
    if (zone_index >= 3 || !zone_confirmed_[zone_index] || mode < 0x05 || mode > 0x08) return false;
    uint8_t block[kZoneBlockSize];
    memcpy(block, zone_block_[zone_index], kZoneBlockSize);
    uint8_t owned = 0;
    switch (mode) {
      case 0x05: owned = 0x04 | (block[4] & 0x01); break;
      case 0x06: owned = 0x01; break;
      case 0x07: owned = 0x00; break;
      default: owned = 0x10; break;
    }
    block[3] = mode;
    block[4] = static_cast<uint8_t>((block[4] & ~0x17) | owned);
    return request_zone_block(zone_index, block, kZoneFieldMode);
  }
  // Auto with a derogation until the satellite's next programme transition: options 0x04 | 0x02 | comfort, as the
  // MQTT firmware writes it (Zone::setMode). Choosing Auto again clears it.
  bool request_zone_derogation(uint8_t zone_index, bool comfort) {
    if (zone_index >= 3 || !zone_confirmed_[zone_index]) return false;
    uint8_t block[kZoneBlockSize];
    memcpy(block, zone_block_[zone_index], kZoneBlockSize);
    block[3] = 0x05;
    block[4] = static_cast<uint8_t>((block[4] & ~0x17) | 0x06 | (comfort ? 0x01 : 0x00));
    return request_zone_block(zone_index, block, kZoneFieldMode);
  }
  bool request_zone_program(uint8_t zone_index, const uint8_t *program) {
    if (zone_index >= 3 || !zone_confirmed_[zone_index] || !program) return false;
    uint8_t block[kZoneBlockSize];
    memcpy(block, zone_block_[zone_index], kZoneBlockSize);
    memcpy(block + 6, program, 42);
    return request_zone_block(zone_index, block, kZoneFieldProgram);
  }
  bool request_zone_setpoints(uint8_t zone_index, uint8_t comfort_raw, uint8_t reduced_raw, uint8_t frost_raw) {
    if (zone_index >= 3 || !zone_confirmed_[zone_index]) return false;
    uint8_t block[kZoneBlockSize];
    memcpy(block, zone_block_[zone_index], kZoneBlockSize);
    block[0] = comfort_raw;
    block[1] = reduced_raw;
    block[2] = frost_raw;
    return request_zone_block(zone_index, block, kZoneFieldSetpoints);
  }

  // Every validated frame. Returns true when it completes the running read.
  bool on_frame(uint32_t now_ms, const uint8_t *payload, size_t length, ReadResult &result) {
    if (is_zone_relay(payload, length, policy_.self_id) && length <= sizeof(relay_)) {
      memcpy(relay_, payload, length);
      relay_length_ = length;
      ack_pending_ = true;
      const uint8_t index = payload[4] - kIdZone1;
      const uint8_t *relayed = payload + 15;
      bool keep_write = zone_write_pending() && index == zone_index_;
      if (keep_write && same_zone_settings(relayed, zone_request_)) {
        // The satellite already has what we are writing: the write is done (normally the echo comes first).
        zone_state_ = WriteState::kVerified;
        ++stats_.zone_writes_acked;
        keep_write = false;
      } else if (keep_write && !same_zone_settings(relayed, zone_block_[index])) {
        // Neither the old block nor ours: someone changed the zone on the satellite. Local wins, the write is dropped
        // (seen on 31/10/2026: comfort set to 19.0 on the satellite while we were writing 20.0).
        zone_state_ = WriteState::kSuperseded;
        keep_write = false;
      }
      if (!keep_write && zone_writing_ && !zone_write_pending()) {
        zone_writing_ = false;
        transaction_.abort();
      }
      // While our write is pending, a relay of the old block changes nothing.
      if (!keep_write && index < 3) {
        // The boiler repeats a relay until it is acknowledged: persist only what is new.
        if (!zone_known_[index] || memcmp(zone_block_[index], payload + 15, kZoneBlockSize) != 0) zone_dirty_ = true;
        memcpy(zone_block_[index], payload + 15, kZoneBlockSize);
        zone_known_[index] = true;
        zone_confirmed_[index] = true;
      }
    }
    if (zone_write_pending() && is_zone_write_reply_(payload, length)) {
      if (memcmp(zone_block_[zone_index_], zone_request_, kZoneBlockSize) != 0) zone_dirty_ = true;
      memcpy(zone_block_[zone_index_], zone_request_, kZoneBlockSize);
      zone_known_[zone_index_] = true;
      zone_confirmed_[zone_index_] = true;
      zone_state_ = WriteState::kVerified;
      ++stats_.zone_writes_acked;
      if (zone_writing_) {
        zone_writing_ = false;
        transaction_.abort();
      }
      return false;
    }
    if (transaction_.active() && transaction_.on_frame(payload, length)) {
      next_id_ = transaction_.next_message_id();
      if (writing_) {
        // The boiler acknowledged the write: read it back before calling it done.
        writing_ = false;
        write_state_ = WriteState::kAcked;
        ++stats_.writes_acked;
        enqueue_front_(ReadKind::kHotWaterMode);
        return false;
      }
      result.kind = current_kind_;
      if (current_kind_ == ReadKind::kExplore) {
        result.address = explore_address_;
        explore_pending_ = false;
      }
      result.length = transaction_.answer_length();
      memcpy(result.frame, transaction_.answer(), result.length);
      if (current_kind_ == ReadKind::kStatus) {
        // 0xA029 carries the boiler's live clock: no need to wait up to 10 minutes for a broadcast after a boot.
        SatelliteBroadcast status;
        if (decode_status_read(result.frame, result.length, status))
          clock_.set_from_bcd(now_ms, status.year, status.month, status.day, status.hour, status.minute, status.second);
      }
      ++stats_.reads_ok;
      if (result.kind == ReadKind::kHotWaterMode) {
        DecodedHotWaterMode decoded;
        if (decode_hot_water_mode(result.frame, result.length, decoded) && decoded.mode != HotWaterMode::kUnknown) {
          hot_water_mode_ = decoded.mode;
          hot_water_frame_bits_ = decoded.frame_bits;
          hot_water_known_ = true;
          if (write_state_ == WriteState::kAcked) {
            const bool same = decoded.mode == requested_mode_;
            write_state_ = same ? WriteState::kVerified : WriteState::kFailed;
            if (same) ++stats_.writes_verified;
            else ++stats_.writes_failed;
          }
        } else if (write_state_ == WriteState::kAcked) {
          write_state_ = WriteState::kFailed;
          ++stats_.writes_failed;
        }
      }
      return true;
    }
    (void)now_ms;
    return false;
  }

  // Call often. True when `tx` holds a frame whose verdict says how to treat it (kSend: transmit;
  // kDryRun: log only; blocked verdicts: counted, never transmitted).
  bool poll(uint32_t now_ms, TxRequest &tx) {
    if (ack_pending_) {
      ack_pending_ = false;
      tx.length = build_zone_ack(relay_, relay_length_, policy_.self_id, tx.frame, sizeof(tx.frame));
      if (tx.length == 0) return false;
      tx.verdict = evaluate_tx(policy_, tx.frame, tx.length, relay_, relay_length_, &tx.kind);
      if (tx.verdict == TxVerdict::kSend) ++stats_.acks_sent;
      else if (tx.verdict == TxVerdict::kDryRun) ++stats_.dry_runs;
      else ++stats_.acks_blocked;
      return true;
    }

    if (!transaction_.active()) {
      schedule_(now_ms);
      if (!start_next_(now_ms)) return false;
    }

    size_t length = 0;
    const ReadTransaction::Action action = transaction_.poll(now_ms, tx.frame, sizeof(tx.frame), length);
    if (action == ReadTransaction::Action::kFailed) {
      if (zone_writing_) {
        // An unanswered burst is not a failure: the satellite may not have been listening. Try again later.
        zone_writing_ = false;
        zone_next_burst_ms_ = now_ms + kZoneBurstGapMs;
        next_id_ = transaction_.next_message_id();
        transaction_.abort();
        return false;
      }
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kFailed;
        ++stats_.writes_failed;
      } else {
        ++stats_.reads_failed;
        end_explore_failed_();
        if (write_state_ == WriteState::kAcked) {
          // The read-back itself failed: the write stays unverified.
          write_state_ = WriteState::kFailed;
          ++stats_.writes_failed;
        }
      }
      next_id_ = transaction_.next_message_id();
      transaction_.abort();
      return false;
    }
    if (action != ReadTransaction::Action::kSend) return false;
    tx.length = length;
    // A zone write may only change the fields it was requested for, whatever the policy would allow.
    TxPolicy gate = policy_;
    if (zone_writing_) gate.zone_write_mask &= zone_fields_;
    tx.verdict = evaluate_tx(gate, tx.frame, tx.length, nullptr, 0, &tx.kind,
                             zone_writing_ && zone_confirmed_[zone_index_] ? zone_block_[zone_index_] : nullptr);
    if (tx.verdict == TxVerdict::kDryRun) {
      // Nothing will be transmitted: do not wait for an answer.
      ++stats_.dry_runs;
      if (!writing_ && !zone_writing_) end_explore_failed_();
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kNotSent;
      }
      if (zone_writing_) {
        zone_writing_ = false;
        zone_state_ = WriteState::kNotSent;
      }
      transaction_.abort();
    } else if (tx.verdict != TxVerdict::kSend) {
      ++stats_.blocked;
      if (!writing_ && !zone_writing_) end_explore_failed_();
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kNotSent;
      }
      if (zone_writing_) {
        zone_writing_ = false;
        zone_state_ = WriteState::kNotSent;
      }
      transaction_.abort();
    }
    return true;
  }

  bool reading() const { return transaction_.active(); }

  // One exploratory read at a time, of any address, when the policy allows it (YAML `explore: true`).
  bool request_explore_read(uint16_t address, uint16_t words) {
    if (!policy_.explore_reads || words == 0 || words > kMaxExploreWords || explore_pending_) return false;
    explore_address_ = address;
    explore_words_ = words;
    explore_pending_ = true;
    enqueue_(ReadKind::kExplore);
    return true;
  }
  bool explore_pending() const { return explore_pending_; }
  // True once after an exploratory read ended without an answer (unanswered, refused, blocked or dry run).
  bool take_explore_failed() {
    const bool failed = explore_failed_;
    explore_failed_ = false;
    return failed;
  }
  // Whether the last exploratory failure was the boiler's refusal (type 0x83), i.e. an invalid address.
  bool explore_refused() const { return explore_refused_; }
  uint16_t explore_address() const { return explore_address_; }
  uint16_t explore_words() const { return explore_words_; }

 private:
  static constexpr size_t kQueue = 8;

  void end_explore_failed_() {
    if (current_kind_ != ReadKind::kExplore || !explore_pending_) return;
    explore_pending_ = false;
    explore_failed_ = true;
    explore_refused_ = transaction_.refused();
  }

  void enqueue_(ReadKind kind) {
    for (size_t i = 0; i < queue_length_; ++i)
      if (queue_[i] == kind) return;  // already waiting
    if (queue_length_ < kQueue) queue_[queue_length_++] = kind;
  }

  // The read-back of a write must not wait behind scheduled reads.
  void enqueue_front_(ReadKind kind) {
    size_t kept = 0;
    ReadKind rest[kQueue];
    for (size_t i = 0; i < queue_length_; ++i)
      if (queue_[i] != kind) rest[kept++] = queue_[i];
    queue_[0] = kind;
    for (size_t i = 0; i < kept && i + 1 < kQueue; ++i) queue_[i + 1] = rest[i];
    queue_length_ = kept + 1 < kQueue ? kept + 1 : kQueue;
  }

  // Scheduled reads: the three temperature slots of each 10-minute cycle, then the hourly ones.
  void schedule_(uint32_t now_ms) {
    // The first boiler date replaces the uptime clock: attempts made in the old time base must not
    // block the slots of the new one (seen in the 2026-10-26 dry run: :03 and :06 were skipped).
    if (clock_.known() && !clock_was_known_) {
      clock_was_known_ = true;
      for (SlotSchedule &slot : temperature_slots_) slot.reset();
      for (SlotSchedule &slot : status_slots_) slot.reset();
      consumption_slot_.reset();
      hot_water_slot_.reset();
    }
    const uint32_t seconds = clock_.seconds(now_ms);
    for (size_t i = 0; i < kTemperatureSlots; ++i)
      if (temperature_slots_[i].due(seconds, now_ms, kSlotCycleSeconds, kTemperatureOffsets[i])) {
        enqueue_(ReadKind::kInfo);
        // At :03 also the flue snapshot taken around :x2 (0x79F2 carries the :x7 one): burner state while heating.
        if (policy_.live_body_reads && i == 0) enqueue_(ReadKind::kFlue);
      }
    for (size_t i = 0; i < kStatusSlots; ++i)
      if (status_slots_[i].due(seconds, now_ms, kSlotCycleSeconds, kStatusOffsets[i])) enqueue_(ReadKind::kStatus);
    if (consumption_slot_.due(seconds, now_ms, kConsumptionPeriod, kConsumptionOffset)) {
      enqueue_(ReadKind::kConsumption);
      enqueue_(ReadKind::kAnnualConsumption);
    }
    if (hot_water_slot_.due(seconds, now_ms, kHotWaterModePeriod, kHotWaterModeOffset))
      enqueue_(ReadKind::kHotWaterMode);
    // The live heating-body temperature (burner detection), outside the satellites' window.
    if (policy_.live_body_reads && !in_quiet_window(seconds) &&
        (!body_started_ || now_ms - last_body_ms_ >= kBodyReadPeriodMs)) {
      body_started_ = true;
      last_body_ms_ = now_ms;
      enqueue_(ReadKind::kBody);
    }
  }

  bool start_next_(uint32_t now_ms) {
    if (zone_state_ == WriteState::kSending && !zone_writing_ && now_ms - zone_start_ms_ > kZoneWriteDeadlineMs) {
      zone_state_ = WriteState::kFailed;
      ++stats_.zone_writes_failed;
    }
    // Reads and writes wait for the end of the satellites' window; acknowledgements never do.
    if (in_quiet_window(clock_.seconds(now_ms))) return false;
    // A requested hot-water change goes before any queued read.
    if (write_state_ == WriteState::kQueued) {
      transaction_.start_hot_water_write(policy_.self_id, policy_.association, next_id_, requested_mode_,
                                         hot_water_frame_bits_);
      writing_ = true;
      write_state_ = WriteState::kSending;
      return true;
    }
    // Then a zone write: one burst every kZoneBurstGapMs, each under a new id, until the boiler answers.
    if (zone_state_ == WriteState::kQueued || (zone_state_ == WriteState::kSending && now_ms >= zone_next_burst_ms_)) {
      if (zone_state_ == WriteState::kQueued) {
        zone_start_ms_ = now_ms;
        zone_first_id_ = next_id_;
        zone_state_ = WriteState::kSending;
      }
      transaction_.start_zone_write(policy_.self_id, policy_.association, next_id_,
                                    static_cast<uint8_t>(kIdZone1 + zone_index_), zone_request_);
      zone_writing_ = true;
      ++stats_.zone_bursts;
      return true;
    }
    if (queue_length_ == 0) return false;
    const ReadKind kind = queue_[0];
    for (size_t i = 1; i < queue_length_; ++i) queue_[i - 1] = queue_[i];
    --queue_length_;
    uint16_t address = 0, words = 0;
    switch (kind) {
      case ReadKind::kInfo: address = kAddrInfo; words = 0x1C; break;
      case ReadKind::kConsumption: address = kAddrConsumption; words = 0x1C; break;
      case ReadKind::kAnnualConsumption: address = kAddrAnnualConsumption; words = 0x18; break;
      case ReadKind::kHotWaterMode: address = kAddrHotWaterMode; words = 0x01; break;
      case ReadKind::kStatus: address = kAddrStatus; words = 0x15; break;
      case ReadKind::kExplore: address = explore_address_; words = explore_words_; break;
      case ReadKind::kBody: address = kAddrBodyLive; words = 0x01; break;
      case ReadKind::kFlue: address = kAddrFlueSnapshot; words = 0x01; break;
      default: return false;
    }
    current_kind_ = kind;
    transaction_.start(policy_.self_id, policy_.association, next_id_, address, words);
    ++stats_.reads_started;
    return true;
  }

  // Boiler echo of the block we wrote: 80->self, reception = zone | 0x80, 0x30, the 48 bytes, under one of the
  // ids used since the write began.
  bool is_zone_write_reply_(const uint8_t *frame, size_t length) const {
    Header h;
    if (length != kZoneAckLength || !parse_header(frame, length, h)) return false;
    if (h.type != kInit || h.source != kIdBoiler || h.destination != policy_.self_id ||
        h.association != policy_.association || h.reception != ((kIdZone1 + zone_index_) | 0x80) || frame[6] != 0x30)
      return false;
    const uint8_t last = zone_writing_ ? next_id_ : static_cast<uint8_t>(next_id_ - 1);
    if (static_cast<uint8_t>(last - h.message_id) > static_cast<uint8_t>(last - zone_first_id_)) return false;
    return memcmp(frame + 7, zone_request_, kZoneBlockSize) == 0;
  }

  static constexpr uint32_t kZoneBurstGapMs = 30000;
  static constexpr uint32_t kZoneWriteDeadlineMs = 20UL * 60UL * 1000UL;

  TxPolicy policy_;
  BoilerClock clock_;
  bool clock_was_known_ = false;
  EngineStats stats_;
  ReadTransaction transaction_;
  ReadKind current_kind_ = ReadKind::kUnknown;
  uint16_t explore_address_ = 0;
  uint16_t explore_words_ = 0;
  bool explore_pending_ = false;
  bool explore_failed_ = false;
  bool explore_refused_ = false;
  bool body_started_ = false;
  uint32_t last_body_ms_ = 0;
  SlotSchedule temperature_slots_[kTemperatureSlots];
  SlotSchedule status_slots_[kStatusSlots];
  SlotSchedule consumption_slot_;
  SlotSchedule hot_water_slot_;
  ReadKind queue_[kQueue] = {};
  size_t queue_length_ = 0;
  uint8_t next_id_ = 1;
  bool writing_ = false;
  WriteState write_state_ = WriteState::kIdle;
  HotWaterMode requested_mode_ = HotWaterMode::kUnknown;
  HotWaterMode hot_water_mode_ = HotWaterMode::kUnknown;
  uint8_t hot_water_frame_bits_ = 0;
  bool hot_water_known_ = false;
  bool ack_pending_ = false;
  uint8_t zone_block_[3][kZoneBlockSize] = {};
  bool zone_known_[3] = {};
  bool zone_confirmed_[3] = {};
  bool zone_dirty_ = false;
  uint8_t zone_request_[kZoneBlockSize] = {};
  uint8_t zone_index_ = 0;
  uint8_t zone_fields_ = 0;
  uint8_t zone_first_id_ = 0;
  bool zone_writing_ = false;
  WriteState zone_state_ = WriteState::kIdle;
  uint32_t zone_start_ms_ = 0;
  uint32_t zone_next_burst_ms_ = 0;
  uint8_t relay_[64] = {};
  size_t relay_length_ = 0;
};

}  // namespace frisquet
