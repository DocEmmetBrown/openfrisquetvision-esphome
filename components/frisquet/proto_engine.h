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
};

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

  // Every validated frame. Returns true when it completes the running read.
  bool on_frame(uint32_t now_ms, const uint8_t *payload, size_t length, ReadResult &result) {
    if (is_zone_relay(payload, length, policy_.self_id) && length <= sizeof(relay_)) {
      memcpy(relay_, payload, length);
      relay_length_ = length;
      ack_pending_ = true;
    }
    if (transaction_.active() && transaction_.on_frame(payload, length)) {
      result.kind = current_kind_;
      result.length = transaction_.answer_length();
      memcpy(result.frame, transaction_.answer(), result.length);
      ++stats_.reads_ok;
      next_id_ = transaction_.next_message_id();
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
      ++stats_.reads_failed;
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
      transaction_.abort();
    } else if (tx.verdict != TxVerdict::kSend) {
      ++stats_.blocked;
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

  // Scheduled reads: the three temperature slots of each 10-minute cycle, then the hourly ones.
  void schedule_(uint32_t now_ms) {
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
    // Reads wait for the end of the satellites' window; acknowledgements never do.
    if (queue_length_ == 0 || in_quiet_window(clock_.seconds(now_ms))) return false;
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
  EngineStats stats_;
  ReadTransaction transaction_;
  ReadKind current_kind_ = ReadKind::kUnknown;
  SlotSchedule temperature_slots_[3];
  SlotSchedule consumption_slot_;
  SlotSchedule hot_water_slot_;
  ReadKind queue_[kQueue] = {};
  size_t queue_length_ = 0;
  uint8_t next_id_ = 1;
  bool ack_pending_ = false;
  uint8_t relay_[64] = {};
  size_t relay_length_ = 0;
};

}  // namespace frisquet
