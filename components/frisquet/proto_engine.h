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
};

// Life of a hot-water mode write: queued, sent until the boiler acknowledges, then read back.
enum class WriteState : uint8_t { kIdle, kQueued, kSending, kAcked, kVerified, kFailed, kNotSent };

struct TxRequest {
  uint8_t frame[64] = {};
  size_t length = 0;
  TxVerdict verdict = TxVerdict::kBlockedLevel;
  TxClass kind = TxClass::kUnknown;
};

struct ReadResult {
  ReadKind kind = ReadKind::kUnknown;
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

  // Every validated frame. Returns true when it completes the running read.
  bool on_frame(uint32_t now_ms, const uint8_t *payload, size_t length, ReadResult &result) {
    if (is_zone_relay(payload, length, policy_.self_id) && length <= sizeof(relay_)) {
      memcpy(relay_, payload, length);
      relay_length_ = length;
      ack_pending_ = true;
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
      result.length = transaction_.answer_length();
      memcpy(result.frame, transaction_.answer(), result.length);
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
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kFailed;
        ++stats_.writes_failed;
      } else {
        ++stats_.reads_failed;
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
    tx.verdict = evaluate_tx(policy_, tx.frame, tx.length, nullptr, 0, &tx.kind);
    if (tx.verdict == TxVerdict::kDryRun) {
      // Nothing will be transmitted: do not wait for an answer.
      ++stats_.dry_runs;
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kNotSent;
      }
      transaction_.abort();
    } else if (tx.verdict != TxVerdict::kSend) {
      ++stats_.blocked;
      if (writing_) {
        writing_ = false;
        write_state_ = WriteState::kNotSent;
      }
      transaction_.abort();
    }
    return true;
  }

  bool reading() const { return transaction_.active(); }

 private:
  static constexpr size_t kQueue = 8;

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
      consumption_slot_.reset();
      hot_water_slot_.reset();
    }
    const uint32_t seconds = clock_.seconds(now_ms);
    for (size_t i = 0; i < 3; ++i)
      if (temperature_slots_[i].due(seconds, now_ms, kSlotCycleSeconds, kTemperatureOffsets[i]))
        enqueue_(ReadKind::kInfo);
    if (consumption_slot_.due(seconds, now_ms, kConsumptionPeriod, kConsumptionOffset)) {
      enqueue_(ReadKind::kConsumption);
      enqueue_(ReadKind::kAnnualConsumption);
    }
    if (hot_water_slot_.due(seconds, now_ms, kHotWaterModePeriod, kHotWaterModeOffset))
      enqueue_(ReadKind::kHotWaterMode);
  }

  bool start_next_(uint32_t now_ms) {
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
      default: return false;
    }
    current_kind_ = kind;
    transaction_.start(policy_.self_id, policy_.association, next_id_, address, words);
    ++stats_.reads_started;
    return true;
  }

  TxPolicy policy_;
  BoilerClock clock_;
  bool clock_was_known_ = false;
  EngineStats stats_;
  ReadTransaction transaction_;
  ReadKind current_kind_ = ReadKind::kUnknown;
  SlotSchedule temperature_slots_[3];
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
  uint8_t relay_[64] = {};
  size_t relay_length_ = 0;
};

}  // namespace frisquet
