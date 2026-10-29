#include "frisquet_radio.h"

#include <SPI.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio";

static_assert(kSensorCount == FRISQUET_SENSOR_COUNT, "SensorId must match SENSORS in __init__.py");
static_assert(kTextCount == FRISQUET_TEXT_COUNT, "TextId must match TEXT_SENSORS in __init__.py");

// Heltec WiFi LoRa 32 V4.3 front-end module (KCT8103L).
static constexpr uint8_t FEM_POWER_PIN = 7;
static constexpr uint8_t FEM_ENABLE_PIN = 2;
static constexpr uint8_t FEM_CTX_PIN = 5;

// The MQTT firmware waits 30 ms before every transmission; so do we.
static constexpr uint32_t TX_DELAY_MS = 30;

static volatile bool g_rx_flag = false;
// The receive interrupt pin also signals "transmit done": ignore it while transmitting.
static volatile bool g_ignore_irq = false;
static void IRAM_ATTR on_rx_irq() {
  if (!g_ignore_irq) g_rx_flag = true;
}

int16_t RawRadio::arm() {
  int16_t state = standby();
  if (state != RADIOLIB_ERR_NONE) return state;
  if (getPacketType() != RADIOLIB_SX126X_PACKET_TYPE_GFSK) return RADIOLIB_ERR_WRONG_MODEM;
  // Keep the length byte and CRC bytes as received: they are checked in software.
  state = setCRC(0);
  if (state != RADIOLIB_ERR_NONE) return state;
  state = fixedPacketLengthMode(::frisquet::crc::kRawLength);
  if (state != RADIOLIB_ERR_NONE) return state;
  return SX1262::startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                              RADIOLIB_IRQ_RX_DEFAULT_MASK, ::frisquet::crc::kRawLength);
}

int16_t RawRadio::read_raw(uint8_t *raw, size_t &length, uint16_t &irq) {
  length = 0;
  int16_t state = standby();
  if (state != RADIOLIB_ERR_NONE) return state;
  irq = getIrqFlags();
  if (!(irq & (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)))
    return RADIOLIB_ERR_RX_TIMEOUT;
  // In continuous RX the packet may start at any buffer offset: ask the chip.
  uint8_t status[2] = {0, 0};
  state = getMod()->SPIreadStream(RADIOLIB_SX126X_CMD_GET_RX_BUFFER_STATUS, status, 2);
  if (state != RADIOLIB_ERR_NONE) return state;
  const size_t count = status[0] < ::frisquet::crc::kRawLength ? status[0] : ::frisquet::crc::kRawLength;
  state = readBuffer(raw, static_cast<uint8_t>(count), status[1]);
  if (state != RADIOLIB_ERR_NONE) return state;
  length = count;
  return clearIrqStatus();
}

#ifdef FRISQUET_TX_ENABLED
int16_t RawRadio::transmit_frame(const uint8_t *data, size_t length) {
  int16_t state = standby();
  if (state != RADIOLIB_ERR_NONE) return state;
  state = setCRC(2, 0x1D0F, 0x1021, true);
  if (state != RADIOLIB_ERR_NONE) return state;
  state = variablePacketLengthMode(::frisquet::crc::kMaxPayload);
  if (state != RADIOLIB_ERR_NONE) return state;
  return SX1262::transmit(data, length);
}
#endif

void FrisquetRadio::set_kill_switch(bool killed) {
  this->killed_ = killed;
  // An association cannot complete without transmitting: give the network back now. A recovery only listens.
  if (killed && this->pairing_.active() && this->pairing_.kind() == ::frisquet::PairingKind::kAssociate) {
    ESP_LOGW(TAG, "Association cancelled by the emergency stop");
    this->pairing_.cancel();
  }
#ifdef FRISQUET_TX_ENABLED
  this->engine_.set_kill_switch(killed);
#endif
}

namespace {
struct HotWaterName {
  const char *name;
  ::frisquet::HotWaterMode mode;
};
// Same labels and order as HOT_WATER_OPTIONS in select.py.
const HotWaterName HOT_WATER_MODES[] = {
    {"Max", ::frisquet::HotWaterMode::kMax},
    {"Eco", ::frisquet::HotWaterMode::kEco},
    {"Eco Horaires", ::frisquet::HotWaterMode::kEcoScheduled},
    {"Eco+", ::frisquet::HotWaterMode::kEcoPlus},
    {"Eco+ Horaires", ::frisquet::HotWaterMode::kEcoPlusScheduled},
    {"Stop", ::frisquet::HotWaterMode::kStop},
};
}  // namespace

bool FrisquetRadio::request_hot_water_mode(const std::string &name) {
#ifdef FRISQUET_TX_ENABLED
  for (const HotWaterName &entry : HOT_WATER_MODES) {
    if (name == entry.name) return this->engine_.request_hot_water_mode(entry.mode);
  }
#else
  (void) name;
#endif
  return false;
}

void FrisquetRadio::republish_hot_water_mode() {
#ifdef FRISQUET_TX_ENABLED
  this->last_published_mode_ = -1;  // forces sync_hot_water_() to publish the boiler's mode again
#endif
}

#ifdef FRISQUET_TX_ENABLED
static const char *write_state_name(::frisquet::WriteState state) {
  switch (state) {
    case ::frisquet::WriteState::kQueued: return "queued";
    case ::frisquet::WriteState::kSending: return "sending";
    case ::frisquet::WriteState::kAcked: return "acked";
    case ::frisquet::WriteState::kVerified: return "verified";
    case ::frisquet::WriteState::kFailed: return "failed";
    case ::frisquet::WriteState::kNotSent: return "not-sent";
    default: return "idle";
  }
}

bool FrisquetRadio::request_zone_setpoint(uint8_t field, float celsius) {
  if (field > 2 || !this->engine_.zone_block_known(0)) return false;
  uint8_t raw[3];
  memcpy(raw, this->engine_.zone_block(0), 3);
  raw[field] = ::frisquet::encode_temperature8(celsius);
  if (raw[2] > raw[1] || raw[1] > raw[0]) return false;  // frost <= reduced <= comfort
  return this->engine_.request_zone_setpoints(0, raw[0], raw[1], raw[2]);
}

void FrisquetRadio::republish_zone_block() {
  for (int &raw : this->last_zone_raw_) raw = -1;
  this->last_zone_mode_ = -1;
  this->last_program_.clear();
}

namespace {
struct ZoneModeName {
  const char *name;
  uint8_t mode;
};
// Same labels and order as ZONE_MODES in select.py; mode = byte 3 of the zone block.
const ZoneModeName ZONE_MODES[] = {{"Auto", 0x05}, {"Confort", 0x06}, {"Réduit", 0x07}, {"Hors Gel", 0x08}};
}  // namespace

bool FrisquetRadio::request_zone_mode(const std::string &name) {
  for (const ZoneModeName &entry : ZONE_MODES)
    if (name == entry.name) return this->engine_.request_zone_mode(0, entry.mode);
  return false;
}

bool FrisquetRadio::request_zone_program(const std::string &hex) {
  uint8_t program[::frisquet::kProgramSize];
  if (!::frisquet::parse_program_hex(hex.c_str(), hex.size(), program)) return false;
  return this->engine_.request_zone_program(0, program);
}

void FrisquetRadio::restore_zone_blocks_() {
  this->store_pref_ = global_preferences->make_preference<ZoneStoreBlob>(fnv1_hash("frisquet_zone_blocks"));
  ZoneStoreBlob blob;
  uint8_t mask = 0;
  uint8_t blocks[3][::frisquet::kZoneBlockSize] = {};
  if (!this->store_pref_.load(&blob) || !::frisquet::unpack_zone_store(blob.data, sizeof(blob.data), mask, blocks)) {
    ESP_LOGI(TAG, "No stored zone block");
    return;
  }
  for (uint8_t zone = 0; zone < 3; ++zone)
    if (mask & (1 << zone)) this->engine_.restore_zone_block(zone, blocks[zone]);
  ESP_LOGI(TAG, "Restored zone blocks (mask %u): writable once a relay or the satellite's broadcast confirms them", mask);
}

void FrisquetRadio::save_zone_blocks_() {
  ZoneStoreBlob blob;
  ::frisquet::pack_zone_store(this->engine_.zone_known_mask(), this->engine_.zone_blocks(), blob.data);
  if (!this->store_pref_.save(&blob)) ESP_LOGW(TAG, "Saving the zone blocks failed");
  else ESP_LOGI(TAG, "Zone blocks saved (mask %u)", this->engine_.zone_known_mask());
}

void FrisquetRadio::sync_zone_() {
  if (this->engine_.take_zone_dirty()) this->save_zone_blocks_();
  const int block_state = !this->engine_.zone_block_known(0) ? 0 : (this->engine_.zone_block_confirmed(0) ? 2 : 1);
  if (block_state != this->last_block_state_) {
    this->last_block_state_ = block_state;
    static const char *const names[3] = {"inconnu", "restauré (non confirmé)", "confirmé"};
    ESP_LOGI(TAG, "Zone 1 block: %s", names[block_state]);
    this->publish_text_(kZoneBlockState, names[block_state]);
  }
  const ::frisquet::WriteState state = this->engine_.zone_write_state();
  if (state != this->last_zone_state_) {
    this->last_zone_state_ = state;
    ESP_LOGI(TAG, "Zone block write: %s", write_state_name(state));
    this->publish_text_(kZoneWrite, write_state_name(state));
  }
  // The command select follows the zone block once it is confirmed; before that (unknown, or restored and maybe stale)
  // it follows the mode the satellite itself broadcasts.
  const bool block_confirmed = this->engine_.zone_block_confirmed(0);
  const int select_mode = block_confirmed ? this->engine_.zone_block(0)[3] : this->broadcast_zone_mode_;
  if (select_mode > 0 && select_mode != this->last_zone_mode_) {
    this->last_zone_mode_ = select_mode;
#ifdef USE_SELECT
    if (this->zone_mode_select_ != nullptr) {
      for (const ZoneModeName &entry : ZONE_MODES)
        if (entry.mode == select_mode) this->zone_mode_select_->publish_state(entry.name);
    }
#endif
  }
  if (!this->engine_.zone_block_known(0)) return;
  const uint8_t *block = this->engine_.zone_block(0);
  for (uint8_t field = 0; field < 3; ++field) {
    if (block[field] == this->last_zone_raw_[field]) continue;
    this->last_zone_raw_[field] = block[field];
#ifdef USE_NUMBER
    if (this->zone_numbers_[field] != nullptr)
      this->zone_numbers_[field]->publish_state(::frisquet::temperature8(block[field]));
#endif
  }
  char hex[::frisquet::kProgramSize * 2 + 1];
  ::frisquet::format_program_hex(block + 6, hex);
  if (this->last_program_ != hex) {
    this->last_program_ = hex;
#ifdef USE_TEXT
    if (this->program_text_ != nullptr) this->program_text_->publish_state(hex);
#endif
    char ranges[200];
    ::frisquet::format_program_ranges(block + 6, ranges, sizeof(ranges));
    this->publish_text_(kZoneProgram, ranges);
  }
}

void FrisquetRadio::sync_hot_water_() {
  const ::frisquet::WriteState state = this->engine_.write_state();
  if (state != this->last_write_state_) {
    this->last_write_state_ = state;
    ESP_LOGI(TAG, "Hot-water mode write: %s", write_state_name(state));
    this->publish_text_(kHotWaterWrite, write_state_name(state));
  }
  if (this->engine_.hot_water_mode_known()) {
    const int mode = static_cast<int>(this->engine_.hot_water_mode());
    if (mode != this->last_published_mode_) {
      this->last_published_mode_ = mode;
#ifdef USE_SELECT
      if (this->hot_water_select_ != nullptr) {
        for (const HotWaterName &entry : HOT_WATER_MODES) {
          if (static_cast<int>(entry.mode) == mode) {
            this->hot_water_select_->publish_state(entry.name);
            break;
          }
        }
      }
#endif
    }
  }
}
#else
bool FrisquetRadio::request_zone_setpoint(uint8_t, float) { return false; }
void FrisquetRadio::republish_zone_block() {}
bool FrisquetRadio::request_zone_mode(const std::string &) { return false; }
bool FrisquetRadio::request_zone_program(const std::string &) { return false; }
#endif

static std::string format_network(const uint8_t *n) {
  char text[12];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X", n[0], n[1], n[2], n[3]);
  return text;
}

bool FrisquetRadio::start_network_recovery() {
  if (this->radio_ == nullptr || this->is_failed()) return false;
  if (!this->pairing_.start(::frisquet::PairingKind::kRecoverNetwork, ::frisquet::kIdConnect, this->active_network_,
                            millis())) {
    ESP_LOGW(TAG, "NetworkID recovery refused (a pairing session is already running)");
    return false;
  }
  ESP_LOGI(TAG, "NetworkID recovery: listening on the broadcast network for %u s (boiler in pairing mode)",
           static_cast<unsigned>(::frisquet::kPairingListenMs / 1000));
#ifdef FRISQUET_TX_ENABLED
  this->pending_tx_valid_ = false;
#endif
  this->apply_network_();
  return true;
}

bool FrisquetRadio::start_association() {
#ifdef FRISQUET_TX_ENABLED
  if (this->radio_ == nullptr || this->is_failed()) return false;
  if (!this->pairing_allowed_) {
    ESP_LOGW(TAG, "Association refused: 'pairing: true' is not set in the YAML");
    return false;
  }
  if (this->killed_) {
    ESP_LOGW(TAG, "Association refused: the emergency stop is engaged");
    return false;
  }
  if (!this->pairing_.start(::frisquet::PairingKind::kAssociate, ::frisquet::kIdConnect, this->active_network_,
                            millis())) {
    ESP_LOGW(TAG, "Association refused (a pairing session is already running)");
    return false;
  }
  ESP_LOGI(TAG, "Association: listening on the broadcast network for %u s (boiler in pairing mode)",
           static_cast<unsigned>(::frisquet::kPairingListenMs / 1000));
  this->pending_tx_valid_ = false;
  this->apply_network_();
  return true;
#else
  ESP_LOGW(TAG, "Association needs a build with a transmit path (tx: read, ack or write)");
  return false;
#endif
}

bool FrisquetRadio::forget_network() {
  if (this->pairing_.active()) {
    ESP_LOGW(TAG, "Forget refused: a pairing session is running");
    return false;
  }
  NetworkStoreBlob blob{};
  this->network_pref_.save(&blob);  // all zeros: rejected by unpack_network_store()
  global_preferences->sync();
  memcpy(this->active_network_, this->network_id_, 4);
  this->network_learned_ = false;
#ifdef FRISQUET_TX_ENABLED
  if (this->association_ != this->yaml_association_) {
    this->association_ = this->yaml_association_;
    ::frisquet::TxPolicy policy = this->engine_.policy();
    policy.association = this->association_;
    this->engine_.set_policy(policy);
  }
#endif
  ESP_LOGI(TAG, "Learned network forgotten: back to the YAML NetworkID %s", format_network(this->network_id_).c_str());
  this->apply_network_();
  this->publish_network_();
  return true;
}

void FrisquetRadio::apply_network_() {
  const uint8_t *wanted = this->pairing_.active() ? this->pairing_.network() : this->active_network_;
  if (memcmp(wanted, this->applied_network_, 4) == 0) return;
  this->radio_->standby();
  const int16_t state = this->radio_->setSyncWord(const_cast<uint8_t *>(wanted), 4);
  if (state != RADIOLIB_ERR_NONE) {
    // Leave applied_network_ unchanged: the next loop tries again.
    ESP_LOGE(TAG, "setSyncWord(%s) failed: %d", format_network(wanted).c_str(), state);
  } else {
    memcpy(this->applied_network_, wanted, 4);
    ESP_LOGI(TAG, "Radio network: %s", format_network(wanted).c_str());
  }
  this->rearm_("network change");
}

void FrisquetRadio::publish_network_() {
  char text[48];
  snprintf(text, sizeof(text), "%s (%s), association %02X", format_network(this->active_network_).c_str(),
           this->network_learned_ ? "appris" : "YAML", this->association_);
  this->publish_text_(kNetwork, text);
}

void FrisquetRadio::save_network_(uint8_t association) {
  NetworkStoreBlob blob;
  ::frisquet::pack_network_store(this->active_network_, association, blob.data);
  // Written to flash now: the boiler already uses this identity, a power cut must not lose it.
  if (!this->network_pref_.save(&blob) || !global_preferences->sync()) ESP_LOGW(TAG, "Saving the network failed");
}

void FrisquetRadio::pairing_step_(uint32_t now) {
  if (this->pairing_.active()) {
    uint8_t frame[::frisquet::kAssociationConfirmLength];
    size_t length = 0;
    if (this->pairing_.poll(now, frame, length) && length > 0) {
#ifdef FRISQUET_TX_ENABLED
      ::frisquet::TxPolicy policy = this->engine_.policy();
      policy.pairing = this->pairing_allowed_;
      const ::frisquet::TxVerdict verdict = ::frisquet::evaluate_tx(
          policy, frame, length, this->pairing_.offer(), this->pairing_.offer_length());
      ESP_LOGI(TAG, "TX %s association-confirm %s", ::frisquet::verdict_name(verdict),
               format_hex_pretty(frame, length).c_str());
      if (verdict == ::frisquet::TxVerdict::kSend) {
        g_ignore_irq = true;
        const int16_t state = this->radio_->transmit_frame(frame, length);
        g_ignore_irq = false;
        g_rx_flag = false;
        if (state != RADIOLIB_ERR_NONE) ESP_LOGW(TAG, "transmit failed: %d", state);
        this->rearm_("after association confirm");
      } else {
        // Dry run or refused: the boiler gets no confirmation, so nothing may change on our side either.
        ESP_LOGW(TAG, "Association not confirmed (%s): staying on %s", ::frisquet::verdict_name(verdict),
                 format_network(this->active_network_).c_str());
        this->pairing_.cancel();
      }
#else
      this->pairing_.cancel();
#endif
    }
  }

  const ::frisquet::PairingState state = this->pairing_.state();
  if (state == ::frisquet::PairingState::kSucceeded) {
    memcpy(this->active_network_, this->pairing_.network(), 4);
    this->network_learned_ = true;
    uint8_t stored_association = 0xFF;
#ifdef FRISQUET_TX_ENABLED
    if (this->pairing_.kind() == ::frisquet::PairingKind::kAssociate) {
      this->association_ = this->pairing_.association();
      stored_association = this->association_;
      ::frisquet::TxPolicy policy = this->engine_.policy();
      policy.association = this->association_;
      this->engine_.set_policy(policy);
    } else if (this->association_ != this->yaml_association_) {
      stored_association = this->association_;  // keep a previously learned association
    }
#endif
    this->save_network_(stored_association);
    ESP_LOGI(TAG, "Pairing succeeded: NetworkID %s, association %02X", format_network(this->active_network_).c_str(),
             this->association_);
  }
  this->apply_network_();
  if (static_cast<int>(state) != this->last_pairing_state_ && state != ::frisquet::PairingState::kIdle) {
    this->last_pairing_state_ = static_cast<int>(state);
    ESP_LOGI(TAG, "Pairing: %s", ::frisquet::pairing_state_name(state));
    this->publish_text_(kPairingState, ::frisquet::pairing_state_name(state));
    if (!this->pairing_.active()) this->publish_network_();
  }
  if (!this->pairing_.active() && state != ::frisquet::PairingState::kIdle) {
    this->pairing_.acknowledge();
    this->last_pairing_state_ = -1;
  }
}

void FrisquetRadio::setup() {
  if (this->fem_) {
    pinMode(FEM_POWER_PIN, OUTPUT);
    digitalWrite(FEM_POWER_PIN, HIGH);
    pinMode(FEM_ENABLE_PIN, OUTPUT);
    digitalWrite(FEM_ENABLE_PIN, HIGH);
  }
  SPI.begin(this->sck_, this->miso_, this->mosi_, this->cs_);
  this->radio_ = new RawRadio(new Module(this->cs_, this->irq_, this->reset_, this->busy_));

  if (this->fem_) {
    if (this->lna_bypass_) {
      // CTX high selects the RX bypass; RadioLib keeps this table, so it must stay valid.
      static const uint32_t pins[Module::RFSWITCH_MAX_PINS] = {FEM_CTX_PIN, RADIOLIB_NC, RADIOLIB_NC,
                                                               RADIOLIB_NC, RADIOLIB_NC};
      static const Module::RfSwitchMode_t table[] = {
          {Module::MODE_IDLE, {LOW}},
          {Module::MODE_RX, {HIGH}},
          {Module::MODE_TX, {HIGH}},
          END_OF_MODE_TABLE,
      };
      this->radio_->setRfSwitchTable(pins, table);
    } else {
      // CTX low in RX keeps the LNA in the path, high in TX selects the PA.
      this->radio_->setRfSwitchPins(RADIOLIB_NC, FEM_CTX_PIN);
    }
  }

  int16_t state = this->radio_->beginFSK();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "beginFSK failed: %d", state);
    this->mark_failed();
    return;
  }
  this->radio_->setFrequency(868.96);
  this->radio_->setBitRate(25.0);
  this->radio_->setFrequencyDeviation(50.0);
  state = this->radio_->setRxBandwidth(this->rx_bandwidth_);
  if (state != RADIOLIB_ERR_NONE) ESP_LOGW(TAG, "setRxBandwidth(%.1f) failed: %d", this->rx_bandwidth_, state);
  this->radio_->setPreambleLength(16);
  memcpy(this->active_network_, this->network_id_, 4);
  this->network_pref_ = global_preferences->make_preference<NetworkStoreBlob>(fnv1_hash("frisquet_network"));
  {
    NetworkStoreBlob blob;
    uint8_t network[4];
    uint8_t association = 0xFF;
    if (this->network_pref_.load(&blob) &&
        ::frisquet::unpack_network_store(blob.data, sizeof(blob.data), network, association)) {
      memcpy(this->active_network_, network, 4);
      this->network_learned_ = true;
#ifdef FRISQUET_TX_ENABLED
      if (association != 0xFF) this->association_ = association;
#endif
      ESP_LOGI(TAG, "Using the learned NetworkID %s (YAML: %s)", format_network(network).c_str(),
               format_network(this->network_id_).c_str());
    }
  }
  state = this->radio_->setSyncWord(this->active_network_, 4);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "setSyncWord failed: %d", state);
    this->mark_failed();
    return;
  }
  memcpy(this->applied_network_, this->active_network_, 4);
  this->radio_->setPacketReceivedAction(on_rx_irq);

#ifdef FRISQUET_TX_ENABLED
  ::frisquet::TxPolicy policy;
  policy.level = this->tx_level_ >= 3   ? ::frisquet::TxLevel::kWrite
                 : this->tx_level_ >= 2 ? ::frisquet::TxLevel::kAck
                                        : ::frisquet::TxLevel::kRead;
  policy.dry_run = this->dry_run_;
  policy.kill_switch = this->killed_;
  policy.self_id = ::frisquet::kIdConnect;
  policy.association = this->association_;
  if (this->tx_level_ >= 3)
    policy.zone_write_mask = ::frisquet::kZoneFieldSetpoints | ::frisquet::kZoneFieldMode | ::frisquet::kZoneFieldProgram;
  this->engine_.set_policy(policy);
  this->engine_.set_next_message_id(static_cast<uint8_t>(random_uint32() & 0xFF));
  this->engine_.request_initial_reads();
  this->restore_zone_blocks_();
#endif
  this->rearm_("setup");
  this->publish_network_();
}

void FrisquetRadio::rearm_(const char *reason) {
  const int16_t state = this->radio_->arm();
  this->last_arm_ms_ = millis();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "re-arm (%s) failed: %d", reason, state);
  } else {
    ESP_LOGV(TAG, "RX armed (%s)", reason);
  }
}

void FrisquetRadio::loop() {
  if (this->radio_ == nullptr || this->is_failed()) return;
  const uint32_t now = millis();
  if (g_rx_flag) {
    g_rx_flag = false;
    this->handle_packet_();
    this->rearm_("after packet");
  } else if (now - this->last_arm_ms_ > 30000) {
    // Safety net against a lost interrupt or a chip that left RX.
    this->rearm_("watchdog");
  }
  this->pairing_step_(millis());
#ifdef FRISQUET_TX_ENABLED
  this->tx_step_(millis());
#endif
  if (this->frame_seen_ && millis() - this->last_age_publish_ms_ >= 30000) {
    const uint32_t current = millis();
    this->last_age_publish_ms_ = current;
    this->publish_(kSecondsSinceFrame, (current - this->last_frame_ms_) / 1000.0f);
  }
}

#ifdef FRISQUET_TX_ENABLED
static const char *tx_kind_name(::frisquet::TxClass kind) {
  switch (kind) {
    case ::frisquet::TxClass::kReadRequest: return "read";
    case ::frisquet::TxClass::kZoneAck: return "zone-ack";
    case ::frisquet::TxClass::kHotWaterWrite: return "hot-water-write";
    case ::frisquet::TxClass::kZoneWrite: return "zone-write";
    default: return "?";
  }
}

void FrisquetRadio::tx_step_(uint32_t now) {
  this->sync_hot_water_();
  this->sync_zone_();
  // On the broadcast network (or until the installation's sync word is applied again) nothing of ours goes out.
  if (!this->on_installation_network_()) {
    this->pending_tx_valid_ = false;
    return;
  }
  if (this->pending_tx_valid_) {
    if (static_cast<int32_t>(now - this->pending_tx_due_ms_) >= 0) this->transmit_pending_();
    return;
  }
  ::frisquet::TxRequest tx;
  if (this->engine_.poll(now, tx)) {
    // Every frame the engine wants to send is logged with the gate's verdict, sent or not.
    ESP_LOGI(TAG, "TX %s %s %s", ::frisquet::verdict_name(tx.verdict), tx_kind_name(tx.kind),
             format_hex_pretty(tx.frame, tx.length).c_str());
    if (tx.verdict == ::frisquet::TxVerdict::kSend) {
      this->pending_tx_ = tx;
      this->pending_tx_valid_ = true;
      this->pending_tx_due_ms_ = now + TX_DELAY_MS;
    }
  }
  if (now - this->last_stats_ms_ >= 5000) {
    this->last_stats_ms_ = now;
    const ::frisquet::EngineStats &s = this->engine_.stats();
    this->publish_(kAcksSent, s.acks_sent);
    this->publish_(kReadsOk, s.reads_ok);
    this->publish_(kReadsFailed, s.reads_failed);
    this->publish_(kTxDryRuns, s.dry_runs);
    this->publish_(kTxBlocked, s.blocked + s.acks_blocked);
  }
}

void FrisquetRadio::transmit_pending_() {
  this->pending_tx_valid_ = false;
  // The kill switch can be engaged during the 30 ms wait: honour it right before transmitting.
  if (this->killed_) {
    ESP_LOGW(TAG, "TX cancelled by the kill switch");
    return;
  }
  if (!this->on_installation_network_()) {
    ESP_LOGW(TAG, "TX cancelled: the radio is not on the installation's network");
    return;
  }
  g_ignore_irq = true;
  const int16_t state = this->radio_->transmit_frame(this->pending_tx_.frame, this->pending_tx_.length);
  g_ignore_irq = false;
  g_rx_flag = false;
  if (state != RADIOLIB_ERR_NONE) {
    this->tx_errors_++;
    ESP_LOGW(TAG, "transmit failed: %d", state);
  }
  this->rearm_("after tx");
}
#endif

void FrisquetRadio::handle_packet_() {
  uint8_t raw[::frisquet::crc::kRawLength] = {};
  size_t length = 0;
  uint16_t irq = 0;
  const int16_t state = this->radio_->read_raw(raw, length, irq);
  const float rssi = this->radio_->getRSSI();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "read failed: %d (irq=0x%04X)", state, irq);
    return;
  }

  if (length != ::frisquet::crc::kRawLength ||
      (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) ||
      !(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
    this->rejected_++;
    ESP_LOGD(TAG, "RX rejected: len=%u irq=0x%04X rssi=%.1f", static_cast<unsigned>(length), irq, rssi);
    this->publish_counters_();
    return;
  }

  uint8_t payload[::frisquet::crc::kMaxPayload] = {};
  const ::frisquet::crc::Result result = ::frisquet::crc::decode(raw, length, payload);
  if (result.status != ::frisquet::crc::Status::Valid && result.status != ::frisquet::crc::Status::Repaired) {
    this->rejected_++;
    ESP_LOGD(TAG, "RX crc %s rssi=%.1f raw=%s", ::frisquet::crc::status_name(result.status), rssi,
             format_hex_pretty(raw, length).c_str());
    this->publish_counters_();
    return;
  }
  if (result.status == ::frisquet::crc::Status::Repaired) {
    this->repaired_++;
    ESP_LOGD(TAG, "RX repaired bit=%u zero=%+d raw=%s", static_cast<unsigned>(result.bit), result.adjustment,
             format_hex_pretty(raw, length).c_str());
  } else {
    this->valid_++;
  }
  this->last_frame_ms_ = millis();
  this->last_age_publish_ms_ = this->last_frame_ms_;
  this->frame_seen_ = true;
  this->publish_(kSecondsSinceFrame, 0.0f);
  this->publish_(kRssi, rssi);

  ::frisquet::Header header;
  ::frisquet::parse_header(payload, result.length, header);
  ESP_LOGD(TAG, "RX %u bytes %02X->%02X type=%02X rssi=%.1f %s", static_cast<unsigned>(result.length),
           header.source, header.destination, header.type, rssi, format_hex_pretty(payload, result.length).c_str());
  if (this->pairing_.active()) {
    if (this->pairing_.on_frame(millis(), payload, result.length))
      ESP_LOGI(TAG, "Association offer received: %s", format_hex_pretty(payload, result.length).c_str());
    return;  // frames heard on the broadcast network are not the installation's traffic
  }
  if (!this->on_installation_network_()) return;

#ifdef FRISQUET_TX_ENABLED
  // The engine sees every frame: relays to acknowledge, and the answers to its own reads.
  ::frisquet::ReadResult read;
  if (this->engine_.on_frame(millis(), payload, result.length, read)) {
    this->publish_read_(read.kind, read.frame, read.length);
  }
#endif
  this->dispatch_(payload, result.length);
  this->publish_counters_();
}

void FrisquetRadio::dispatch_(const uint8_t *payload, size_t length) {
  ::frisquet::SatelliteBroadcast broadcast;
  if (::frisquet::decode_satellite_broadcast(payload, length, broadcast)) {
    this->publish_satellite_broadcast_(broadcast);
    return;
  }

  // Zone configuration blocks relayed by the boiler (the satellite's own settings and mode).
  ::frisquet::ZoneBlock block;
  if (::frisquet::decode_zone_block(payload, length, block)) {
    char text[96];
    snprintf(text, sizeof(text), "%02X->%02X mode=%02X options=%02X confort=%.1f réduit=%.1f hors-gel=%.1f",
             block.source, block.destination, block.mode, block.options, block.comfort, block.reduced,
             block.frost);
    // The boiler repeats the block every few seconds: publish only changes.
    if (this->last_zone_block_ != text) {
      this->last_zone_block_ = text;
      this->publish_text_(kZoneBlock, text);
    }
#ifndef FRISQUET_TX_ENABLED
    // Listen-only build: show the acknowledgement a Connect would send. Nothing is transmitted.
    uint8_t ack[::frisquet::kZoneAckLength];
    if (::frisquet::build_zone_ack(payload, length, ::frisquet::kIdConnect, ack, sizeof(ack)) > 0) {
      ESP_LOGD(TAG, "DRY-RUN ack (not sent) id=%02X: %s", payload[3], format_hex_pretty(ack, sizeof(ack)).c_str());
    }
#endif
    return;
  }

  // Answers to a Connect's reads, overheard when a real Connect is on the air.
  ::frisquet::ReadKind kind = ::frisquet::ReadKind::kUnknown;
  if (!this->tracker_.on_frame(payload, length, millis(), kind)) {
    this->ignored_++;
    return;
  }
  this->publish_read_(kind, payload, length);
}

void FrisquetRadio::publish_read_(::frisquet::ReadKind kind, const uint8_t *frame, size_t length) {
  switch (kind) {
    case ::frisquet::ReadKind::kInfo: {
      ::frisquet::BoilerInfo info;
      if (::frisquet::decode_boiler_info(frame, length, info)) this->publish_info_(info);
      break;
    }
    case ::frisquet::ReadKind::kConsumption: {
      ::frisquet::Consumption consumption;
      if (::frisquet::decode_consumption(frame, length, consumption)) {
        if (consumption.hot_water >= 0) this->publish_(kConsumptionHotWater, consumption.hot_water);
        if (consumption.heating >= 0) this->publish_(kConsumptionHeating, consumption.heating);
      }
      break;
    }
    case ::frisquet::ReadKind::kAnnualConsumption: {
      ::frisquet::AnnualConsumption annual;
      if (::frisquet::decode_annual_consumption(frame, length, annual)) {
        this->publish_(kAnnualHotWater, annual.hot_water);
        this->publish_(kAnnualHeating, annual.heating);
      }
      break;
    }
    case ::frisquet::ReadKind::kHotWaterMode: {
      ::frisquet::DecodedHotWaterMode mode;
      if (::frisquet::decode_hot_water_mode(frame, length, mode)) {
        const char *name = "Inconnu";
        switch (mode.mode) {
          case ::frisquet::HotWaterMode::kMax: name = "Max"; break;
          case ::frisquet::HotWaterMode::kEco: name = "Eco"; break;
          case ::frisquet::HotWaterMode::kEcoScheduled: name = "Eco Horaires"; break;
          case ::frisquet::HotWaterMode::kEcoPlus: name = "Eco+"; break;
          case ::frisquet::HotWaterMode::kEcoPlusScheduled: name = "Eco+ Horaires"; break;
          case ::frisquet::HotWaterMode::kStop: name = "Stop"; break;
          default: break;
        }
        this->publish_text_(kHotWaterMode, name);
      }
      break;
    }
    default: break;
  }
}

void FrisquetRadio::publish_(SensorId id, float value) {
  if (this->sensors_[id] != nullptr && !std::isnan(value)) this->sensors_[id]->publish_state(value);
}

void FrisquetRadio::publish_text_(TextId id, const char *value) {
  if (this->texts_[id] != nullptr) this->texts_[id]->publish_state(value);
}

void FrisquetRadio::publish_counters_() {
  this->publish_(kFramesValid, this->valid_);
  this->publish_(kFramesRepaired, this->repaired_);
  this->publish_(kFramesRejected, this->rejected_);
}

static const char *zone_mode_name(::frisquet::ZoneMode mode) {
  switch (mode) {
    case ::frisquet::ZoneMode::kReducedPermanent: return "Réduit";
    case ::frisquet::ZoneMode::kComfortPermanent: return "Confort";
    case ::frisquet::ZoneMode::kReducedOverride: return "Dérogation Réduit";
    case ::frisquet::ZoneMode::kComfortOverride: return "Dérogation Confort";
    case ::frisquet::ZoneMode::kReducedAuto: return "Auto (réduit)";
    case ::frisquet::ZoneMode::kComfortAuto: return "Auto (confort)";
    case ::frisquet::ZoneMode::kFrostProtection: return "Hors gel";
    default: return "Inconnu";
  }
}

void FrisquetRadio::publish_satellite_broadcast_(const ::frisquet::SatelliteBroadcast &b) {
  this->publish_(kZone1Ambient, b.ambient[0]);
  this->publish_(kZone1Setpoint, b.setpoint[0]);
  this->publish_(kOutdoor, b.outdoor);
  this->publish_text_(kZone1Mode, zone_mode_name(::frisquet::decode_zone_mode(b.mode_raw[0]).mode));

  this->boiler_state_ = ::frisquet::decode_boiler_state(b.state_raw);
  this->state_known_ = true;
  this->publish_text_(kBoilerState, this->boiler_state_.fault ? "Anomalie"
                                    : (this->boiler_state_.running ? "En marche" : "Veille"));
  // The date is sent as BCD: 0x26 0x10 0x25 means 2026-10-25.
  char text[24];
  snprintf(text, sizeof(text), "20%02X-%02X-%02X %02X:%02X:%02X", b.year, b.month, b.day, b.hour, b.minute,
           b.second);
  this->publish_text_(kBoilerClock, text);
  char raw[40];
  snprintf(raw, sizeof(raw), "état=%02X mode=%02X jour=%u", b.state_raw, b.mode_raw[0], b.weekday);
  this->publish_text_(kZone1Raw, raw);
#ifdef FRISQUET_TX_ENABLED
  // The broadcast date drives the radio slots, as the MQTT firmware's clock does.
  this->engine_.clock().set_from_bcd(millis(), b.year, b.month, b.day, b.hour, b.minute, b.second);
  // A block restored after a reboot is confirmed (or discarded) by what the satellite itself reports.
  this->engine_.on_zone_broadcast(0, b.mode_raw[0], b.setpoint[0], b.weekday, b.hour, b.minute);
  this->broadcast_zone_mode_ = ::frisquet::select_mode_from_broadcast(b.mode_raw[0]);
#endif
  this->publish_activity_();
}

void FrisquetRadio::publish_info_(const ::frisquet::BoilerInfo &info) {
  this->publish_(kZone1Ambient, info.ambient[0]);
  this->publish_(kZone1Setpoint, info.setpoint[0]);
  this->publish_(kHotWater, info.hot_water);
  this->publish_(kHeatingBody, info.heating_body);
  this->publish_(kOutdoor, info.outdoor);
  this->publish_(kFlue, info.flue);
  if (this->burner_sensor_ != nullptr && !isnan(info.flue))
    this->burner_sensor_->publish_state(::frisquet::burner_firing(info.flue));
  this->publish_(kZone1Flow, info.flow[0]);
  this->publish_(kZone1FlowSetpoint, info.flow_setpoint[0]);
  this->publish_(kPressure, info.pressure);
  this->operating_mode_ = ::frisquet::decode_operating_mode(info.operating_mode_raw);
  this->publish_activity_();
}

void FrisquetRadio::publish_activity_() {
  const char *name = "Inconnu";
  switch (::frisquet::boiler_activity(this->state_known_, this->boiler_state_, this->operating_mode_)) {
    case ::frisquet::Activity::kStandby: name = "Veille"; break;
    case ::frisquet::Activity::kHeating: name = "Chauffage"; break;
    case ::frisquet::Activity::kHotWater: name = "Chauffage ECS"; break;
    default: break;
  }
  this->publish_text_(kBoilerActivity, name);
}

void FrisquetRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "Frisquet radio:");
  ESP_LOGCONFIG(TAG, "  Pins: CS=%u IRQ=%u RST=%u BUSY=%u SCK=%u MISO=%u MOSI=%u", this->cs_, this->irq_, this->reset_,
                this->busy_, this->sck_, this->miso_, this->mosi_);
  ESP_LOGCONFIG(TAG, "  Network ID: %s (%s)", format_network(this->active_network_).c_str(),
                this->network_learned_ ? "learned by pairing" : "YAML");
  if (this->network_learned_) ESP_LOGCONFIG(TAG, "  YAML network ID: %s", format_network(this->network_id_).c_str());
  ESP_LOGCONFIG(TAG, "  Association confirmations: %s", this->pairing_allowed_ ? "allowed" : "refused");
  ESP_LOGCONFIG(TAG, "  RX bandwidth: %.1f kHz", this->rx_bandwidth_);
  if (this->fem_) ESP_LOGCONFIG(TAG, "  KCT8103L FEM: LNA bypass %s", this->lna_bypass_ ? "on" : "off");
#ifdef FRISQUET_TX_ENABLED
  ESP_LOGCONFIG(TAG, "  Transmit: %s, association 0x%02X, %s",
                this->tx_level_ >= 3 ? "reads + zone acks + hot-water mode and zone 1 setpoint, mode and programme writes"
                : this->tx_level_ >= 2 ? "reads + zone acks"
                                       : "reads only",
                this->association_, this->dry_run_ ? "DRY RUN (nothing is transmitted)" : "LIVE");
  if (!this->dry_run_) ESP_LOGW(TAG, "  Live transmission is enabled: this device acts as the boiler's Connect.");
#else
  ESP_LOGCONFIG(TAG, "  Transmit: none (receive only, no transmit code compiled)");
#endif
  ESP_LOGCONFIG(TAG, "  Frames: valid=%u repaired=%u rejected=%u", this->valid_, this->repaired_, this->rejected_);
}

}  // namespace frisquet_radio
}  // namespace esphome
