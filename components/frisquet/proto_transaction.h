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
    self_id_ = self_id;
    association_ = association;
    message_id_ = first_message_id;
    address_ = address;
    size_words_ = size_words;
    attempt_ = 0;
    sends_ = 0;
    last_send_ms_ = 0;
    answer_length_ = 0;
    state_ = State::kRunning;
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
    if (sends_ > 0 && now_ms - last_send_ms_ < kSendIntervalMs) return Action::kNone;
    if (sends_ >= kSendsPerAttempt) {
      if (attempt_ + 1 >= kAttempts) {
        state_ = State::kFailed;
        return Action::kFailed;
      }
      ++attempt_;
      ++message_id_;
      sends_ = 0;
    }
    length = build_read_request(frame, capacity, self_id_, association_, message_id_, address_, size_words_);
    if (length == 0) {
      state_ = State::kFailed;
      return Action::kFailed;
    }
    ++sends_;
    last_send_ms_ = now_ms;
    return Action::kSend;
  }

  // Feed every received frame; true when it is the awaited answer.
  bool on_frame(const uint8_t *frame, size_t length) {
    if (state_ != State::kRunning) return false;
    Header h;
    if (!parse_header(frame, length, h)) return false;
    if (h.type != kRead || h.reception != 0x81 || h.source != kIdBoiler || h.destination != self_id_ ||
        h.association != association_ || h.message_id != message_id_)
      return false;
    if (length > sizeof(answer_)) return false;
    memcpy(answer_, frame, length);
    answer_length_ = length;
    state_ = State::kDone;
    return true;
  }

 private:
  enum class State : uint8_t { kIdle, kRunning, kDone, kFailed };
  State state_ = State::kIdle;
  uint8_t self_id_ = kIdConnect;
  uint8_t association_ = 0xFF;
  uint8_t message_id_ = 0;
  uint16_t address_ = 0;
  uint16_t size_words_ = 0;
  uint8_t attempt_ = 0;
  uint8_t sends_ = 0;
  uint32_t last_send_ms_ = 0;
  uint8_t answer_[64] = {};
  size_t answer_length_ = 0;
};

}  // namespace frisquet
