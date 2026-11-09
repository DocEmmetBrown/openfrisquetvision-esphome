#pragma once
// One read transaction, as a pure state machine driven by the caller's clock. It reproduces the
// timing of the MQTT firmware, measured on the air: five transmissions 2.2 s apart under one
// message id, then a second attempt under the next id, failing 22 s after the start.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "proto_encode.h"

namespace frisquet {

class ReadTransaction {
 public:
  static constexpr uint8_t kSendsPerAttempt = 5;
  static constexpr uint8_t kAttempts = 2;
  static constexpr uint32_t kSendIntervalMs = 2200;

  enum class Action : uint8_t { kNone, kSend, kFailed };

  void start(uint8_t self_id, uint8_t association, uint8_t first_message_id, uint16_t address,
             uint16_t size_words) {
    kind_ = Kind::kRead;
    attempts_ = kAttempts;
    self_id_ = self_id;
    association_ = association;
    message_id_ = first_message_id;
    address_ = address;
    size_words_ = size_words;
    attempt_ = 0;
    sends_ = 0;
    last_send_ms_ = 0;
    answer_length_ = 0;
    refused_ = false;
    state_ = State::kRunning;
  }

  // One burst of a zone block write: five sends under one id. The engine starts a new burst (new id) until the
  // boiler answers, which can take minutes because the satellite only listens around its own slot.
  void start_zone_write(uint8_t self_id, uint8_t association, uint8_t message_id, uint8_t zone_id,
                        const uint8_t *block) {
    start(self_id, association, message_id, 0, 0);
    kind_ = Kind::kZoneWrite;
    zone_id_ = zone_id;
    memcpy(block_, block, kZoneBlockSize);
    attempts_ = 1;
  }

  // Same retransmission pattern for the hot-water mode write; the answer is the boiler's INIT acknowledgement.
  void start_hot_water_write(uint8_t self_id, uint8_t association, uint8_t first_message_id, HotWaterMode mode,
                             uint8_t frame_bits) {
    start(self_id, association, first_message_id, 0, 0);
    kind_ = Kind::kHotWaterWrite;
    mode_ = mode;
    frame_bits_ = frame_bits;
  }

  void start_holiday_write(uint8_t self_id, uint8_t association, uint8_t message_id, uint32_t start_timestamp,
                           uint32_t end_timestamp) {
    start(self_id, association, message_id, 0, 0);
    kind_ = Kind::kHolidayWrite;
    holiday_start_ = start_timestamp;
    holiday_end_ = end_timestamp;
  }

  // A consigne exchange in a satellite name: self_id is the zone id, the answer is the boiler state page.
  void start_satellite_consigne(uint8_t zone_id, uint8_t association, uint8_t first_message_id, const uint8_t *data) {
    start(zone_id, association, first_message_id, 0, 0);
    kind_ = Kind::kSatelliteConsigne;
    memcpy(block_, data, 8);
  }

  // The satellite's block write-back in its name: answered (reception connect | 0x80) once the boiler has forwarded
  // it to the Connect and had it acknowledged.
  void start_satellite_block(uint8_t zone_id, uint8_t association, uint8_t first_message_id, uint8_t connect_id,
                             const uint8_t *block) {
    start(zone_id, association, first_message_id, 0, 0);
    kind_ = Kind::kSatelliteBlock;
    connect_id_ = connect_id;
    memcpy(block_, block, kZoneBlockSize);
  }

  bool active() const { return state_ == State::kRunning; }
  void abort() { state_ = State::kIdle; }
  bool done() const { return state_ == State::kDone; }
  bool failed() const { return state_ == State::kFailed; }
  uint8_t message_id() const { return message_id_; }
  uint8_t next_message_id() const { return static_cast<uint8_t>(message_id_ + 1); }
  const uint8_t *answer() const { return answer_; }
  size_t answer_length() const { return answer_length_; }

  // Call regularly. On kSend, `frame` holds the request to transmit (gate it first).
  Action poll(uint32_t now_ms, uint8_t *frame, size_t capacity, size_t &length) {
    length = 0;
    if (state_ != State::kRunning) return Action::kNone;
    if (refused_) {
      state_ = State::kFailed;
      return Action::kFailed;
    }
    if (sends_ > 0 && now_ms - last_send_ms_ < kSendIntervalMs) return Action::kNone;
    if (sends_ >= kSendsPerAttempt) {
      if (attempt_ + 1 >= attempts_) {
        state_ = State::kFailed;
        return Action::kFailed;
      }
      ++attempt_;
      ++message_id_;
      sends_ = 0;
    }
    switch (kind_) {
      case Kind::kRead:
        length = build_read_request(frame, capacity, self_id_, association_, message_id_, address_, size_words_);
        break;
      case Kind::kHotWaterWrite:
        length = build_hot_water_mode_write(frame, capacity, self_id_, association_, message_id_, mode_, frame_bits_);
        break;
      case Kind::kHolidayWrite:
        length = build_holiday_write(frame, capacity, self_id_, association_, message_id_, holiday_start_, holiday_end_);
        break;
      case Kind::kSatelliteConsigne:
        length = build_satellite_consigne(frame, capacity, self_id_, association_, message_id_, block_);
        break;
      case Kind::kSatelliteBlock:
        length = build_satellite_block(frame, capacity, self_id_, association_, message_id_, connect_id_, block_);
        break;
      default:
        length = build_zone_write(frame, capacity, self_id_, association_, message_id_, zone_id_, block_);
        break;
    }
    if (length == 0) {
      state_ = State::kFailed;
      return Action::kFailed;
    }
    ++sends_;
    last_send_ms_ = now_ms;
    return Action::kSend;
  }

  // Feed every received frame; true when it is the awaited answer. A read refused by the boiler (type 0x83,
  // seen for invalid addresses on 29/10/2026: 7E 80 <assoc> <id> 81 83 02) ends the transaction at the next poll.
  bool on_frame(const uint8_t *frame, size_t length) {
    if (state_ != State::kRunning) return false;
    Header h;
    if (!parse_header(frame, length, h)) return false;
    const uint8_t reply_type = kind_ == Kind::kRead ? kRead : kInit;
    if (kind_ == Kind::kZoneWrite) return false;  // the engine matches the block echo itself
    const uint8_t reply_reception = kind_ == Kind::kSatelliteBlock ? static_cast<uint8_t>(connect_id_ | 0x80) : 0x81;
    if (h.reception != reply_reception || h.source != kIdBoiler || h.destination != self_id_ ||
        h.association != association_ || h.message_id != message_id_)
      return false;
    if (kind_ == Kind::kRead && h.type == (kRead | 0x80)) {
      refused_ = true;
      return false;
    }
    if (h.type != reply_type) return false;
    if (length > sizeof(answer_)) return false;
    memcpy(answer_, frame, length);
    answer_length_ = length;
    state_ = State::kDone;
    return true;
  }

  // True after a failure caused by the boiler's refusal rather than by silence.
  bool refused() const { return refused_; }

 private:
  enum class State : uint8_t { kIdle, kRunning, kDone, kFailed };
  enum class Kind : uint8_t { kRead, kHotWaterWrite, kHolidayWrite, kZoneWrite, kSatelliteConsigne, kSatelliteBlock };
  State state_ = State::kIdle;
  Kind kind_ = Kind::kRead;
  uint8_t connect_id_ = kIdConnect;
  HotWaterMode mode_ = HotWaterMode::kUnknown;
  uint8_t frame_bits_ = 0;
  uint32_t holiday_start_ = 0;
  uint32_t holiday_end_ = 0;
  uint8_t self_id_ = kIdConnect;
  uint8_t association_ = 0xFF;
  uint8_t message_id_ = 0;
  uint8_t zone_id_ = 0;
  uint8_t block_[kZoneBlockSize] = {};
  uint16_t address_ = 0;
  uint16_t size_words_ = 0;
  uint8_t attempts_ = kAttempts;
  uint8_t attempt_ = 0;
  bool refused_ = false;
  uint8_t sends_ = 0;
  uint32_t last_send_ms_ = 0;
  uint8_t answer_[64] = {};
  size_t answer_length_ = 0;
};

}  // namespace frisquet
