#include "frisquet_radio.h"

#include <cmath>
#include <ctime>
#include "zone_climate.h"

#include <SPI.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "proto_clock.h"

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

bool FrisquetRadio::set_holiday_start(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                      uint8_t second) {
  if (!::frisquet::civil_to_timestamp(year, month, day, hour, minute, second, this->holiday_start_)) {
    this->holiday_start_valid_ = false;
    ESP_LOGW(TAG, "Holiday start rejected: invalid or unsupported local date/time");
    return false;
  }
  this->holiday_start_valid_ = true;
  return true;
}

bool FrisquetRadio::set_holiday_end(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                    uint8_t second) {
  if (!::frisquet::civil_to_timestamp(year, month, day, hour, minute, second, this->holiday_end_)) {
    this->holiday_end_valid_ = false;
    ESP_LOGW(TAG, "Holiday end rejected: invalid or unsupported local date/time");
    return false;
  }
  this->holiday_end_valid_ = true;
  return true;
}

bool FrisquetRadio::request_holiday() {
#ifdef FRISQUET_TX_ENABLED
  if (!this->holiday_start_valid_ || !this->holiday_end_valid_ || this->holiday_end_ <= this->holiday_start_) {
    ESP_LOGW(TAG, "Holiday activation refused: set a valid end date/time after the start");
    this->publish_text_(kHolidayWrite, "Refus : choisissez une fin après le début");
    return false;
  }
  if (!this->engine_.request_holiday(this->holiday_start_, this->holiday_end_)) {
    ESP_LOGW(TAG, "Holiday activation refused: waiting for boiler data or another write");
    this->publish_text_(kHolidayWrite, "Refus : lecture chaudière requise ou autre écriture en cours");
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool FrisquetRadio::cancel_holiday() {
#ifdef FRISQUET_TX_ENABLED
  if (!this->engine_.request_holiday(0, 0)) {
    ESP_LOGW(TAG, "Holiday cancellation refused: waiting for boiler data or another write");
    this->publish_text_(kHolidayWrite, "Refus : lecture chaudière requise ou autre écriture en cours");
    return false;
  }
  return true;
#else
  return false;
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
    case ::frisquet::WriteState::kSuperseded: return "superseded";
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
  this->last_boost_ = -1;
  this->last_program_.clear();
}

namespace {
struct ZoneModeName {
  const char *name;
  uint16_t mode;  // byte 3 of the zone block, or a derogation key (select_mode_from_block)
};
// Same labels as ZONE_MODES in select.py.
const ZoneModeName ZONE_MODES[] = {{"Auto", 0x05},
                                   {"Dérogation Confort", ::frisquet::kSelectDerogationComfort},
                                   {"Dérogation Réduit", ::frisquet::kSelectDerogationReduced},
                                   {"Confort", 0x06},
                                   {"Réduit", 0x07},
                                   {"Hors Gel", 0x08}};
}  // namespace

bool FrisquetRadio::request_zone_mode(const std::string &name) {
  for (const ZoneModeName &entry : ZONE_MODES) {
    if (name != entry.name) continue;
    if (entry.mode > 0xFF)
      return this->engine_.request_zone_derogation(0, entry.mode == ::frisquet::kSelectDerogationComfort);
    return this->engine_.request_zone_mode(0, static_cast<uint8_t>(entry.mode));
  }
  return false;
}

bool FrisquetRadio::request_zone_boost(bool on) { return this->engine_.request_zone_boost(0, on); }

const uint8_t *FrisquetRadio::zone1_block() const {
  return this->engine_.zone_block_confirmed(0) ? this->engine_.zone_block(0) : nullptr;
}

bool FrisquetRadio::request_zone_target(float celsius) {
  const uint8_t *block = this->zone1_block();
  if (block == nullptr) return false;
  uint8_t field = 0;
  switch (block[3]) {
    case 0x07: field = 1; break;
    case 0x08: field = 2; break;
    case 0x05: field = (block[4] & 0x01) ? 0 : 1; break;  // Auto: the current period (the satellite's comfort bit)
    default: field = 0; break;
  }
  // In a boost the comfort setpoint carries the extra 1.0 C: set the base, the boost stays.
  if (field == 0 && (block[4] & 0x40)) return false;
  return this->request_zone_setpoint(field, celsius);
}

bool FrisquetRadio::request_zone_program(const std::string &hex) {
  uint8_t program[::frisquet::kProgramSize];
  if (!::frisquet::parse_program_hex(hex.c_str(), hex.size(), program)) return false;
  return this->engine_.request_zone_program(0, program);
}

bool FrisquetRadio::request_zone_profile(const std::string &hex, float comfort, float reduced, float frost) {
  const float temperatures[3] = {comfort, reduced, frost};
  uint8_t raw[3];
  for (size_t i = 0; i < 3; ++i) {
    const float value = temperatures[i];
    if (!std::isfinite(value) || value < 5 || value > (i == 2 ? 15 : 30) || value * 2 != std::floor(value * 2)) {
      ESP_LOGW(TAG, "Profile refused: temperatures must be in range with 0.5 C steps");
      this->publish_text_(kZoneWrite, "refused: invalid profile temperatures");
      return false;
    }
    raw[i] = static_cast<uint8_t>(std::lround(value * 10) - 50);
  }
  uint8_t program[::frisquet::kProgramSize];
  if (!::frisquet::parse_program_hex(hex.c_str(), hex.size(), program) ||
      !this->engine_.request_zone_profile(0, program, raw[0], raw[1], raw[2])) {
    ESP_LOGW(TAG, "Profile refused: invalid programme or setpoints, block unconfirmed, boost/write active, or writes disabled");
    this->publish_text_(kZoneWrite, "refused: invalid profile, unconfirmed block, boost/write active or writes disabled");
    return false;
  }
  ESP_LOGI(TAG, "Profile queued as one zone block; current heating mode preserved");
  return true;
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
  const int select_mode =
      block_confirmed ? ::frisquet::select_mode_from_block(this->engine_.zone_block(0)) : this->broadcast_zone_mode_;
  if (select_mode > 0 && select_mode != this->last_zone_mode_) {
    this->last_zone_mode_ = select_mode;
#ifdef USE_SELECT
    if (this->zone_mode_select_ != nullptr) {
      for (const ZoneModeName &entry : ZONE_MODES)
        if (entry.mode == select_mode) this->zone_mode_select_->publish_state(entry.name);
    }
#endif
  }
  const uint8_t satellite_association = this->engine_.policy().satellite_association;
  if (satellite_association != this->last_satellite_association_) {
    this->last_satellite_association_ = satellite_association;
    if (satellite_association != 0xFF) ESP_LOGI(TAG, "Zone 1 satellite association: %02X", satellite_association);
  }
  const int alive = this->engine_.satellite_alive(millis()) ? 1 : 0;
  if (alive != this->last_satellite_alive_) {
    if (alive) ESP_LOGI(TAG, "Zone 1 satellite heard: changes also go to the boiler directly, in its name");
    else if (this->last_satellite_alive_ == 1)
      ESP_LOGW(TAG, "Zone 1 satellite silent for 25 min: nothing more in its name until it is heard again");
    this->last_satellite_alive_ = alive;
  }
  if (!this->engine_.zone_block_known(0)) return;
  const uint8_t *block = this->engine_.zone_block(0);
  const int boost = block_confirmed && (block[4] & 0x40) ? 1 : 0;
  if (boost != this->last_boost_) {
    if (boost && this->last_boost_ != 1) {
      // After a reboot the start is unknown: count from now (at most 6 h more).
      this->boost_since_ms_ = millis();
      this->boost_stop_sent_ = false;
    }
    this->last_boost_ = boost;
#ifdef USE_SWITCH
    if (this->boost_switch_ != nullptr) this->boost_switch_->publish_state(boost);
#endif
  }
  if (boost && !this->boost_stop_sent_ && millis() - this->boost_since_ms_ >= kBoostMaxMs &&
      this->engine_.request_zone_boost(0, false)) {
    this->boost_stop_sent_ = true;
    ESP_LOGI(TAG, "Zone 1 boost: 6 h reached, stopping it");
  }
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

void FrisquetRadio::sync_holiday_() {
  const ::frisquet::WriteState state = this->engine_.holiday_write_state();
  if (state == this->last_holiday_write_state_) return;
  this->last_holiday_write_state_ = state;
  ESP_LOGI(TAG, "Holiday write: %s", write_state_name(state));
  this->publish_text_(kHolidayWrite, write_state_name(state));
}
#else
bool FrisquetRadio::request_zone_setpoint(uint8_t, float) { return false; }
void FrisquetRadio::republish_zone_block() {}
bool FrisquetRadio::request_zone_mode(const std::string &) { return false; }
bool FrisquetRadio::request_zone_boost(bool) { return false; }
const uint8_t *FrisquetRadio::zone1_block() const { return nullptr; }
bool FrisquetRadio::request_zone_target(float) { return false; }
bool FrisquetRadio::request_zone_program(const std::string &) { return false; }
bool FrisquetRadio::request_zone_profile(const std::string &, float, float, float) {
  ESP_LOGW(TAG, "Profile refused: requires tx: write");
  this->publish_text_(kZoneWrite, "refused: requires tx: write");
  return false;
}
#endif

static std::string format_network(const uint8_t *n) {
  char text[12];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X", n[0], n[1], n[2], n[3]);
  return text;
}

bool FrisquetRadio::request_explore_read(const std::string &request) {
#ifdef FRISQUET_TX_ENABLED
  if (request == "BOOST 1" || request == "BOOST 0") {
    const bool on = request[6] == '1';
    if (!this->engine_.request_zone_boost(0, on)) {
      ESP_LOGW(TAG, "BOOST %s refused (block unknown, not in comfort, already %s)", on ? "on" : "off",
               on ? "on" : "off");
      return false;
    }
    ESP_LOGI(TAG, "BOOST %s queued for zone 1", on ? "on" : "off");
    return true;
  }
  if (request.rfind("SATZ ", 0) == 0) {
    // "SATZ <mode>": the zone 1 satellite's block write-back in its name, the known block with that mode (05..08).
    unsigned mode = 0;
    if (sscanf(request.c_str() + 5, "%x", &mode) != 1 || mode > 0xFF) {
      ESP_LOGW(TAG, "SATZ request refused: expected 'SATZ <mode>' in hexadecimal (05 auto, 06, 07, 08)");
      return false;
    }
    if (!this->engine_.request_satellite_block(static_cast<uint8_t>(mode))) {
      ESP_LOGW(TAG, "SATZ request refused (satellite_override off, block unknown, bad mode, or one pending)");
      return false;
    }
    ESP_LOGI(TAG, "SATZ block queued: mode %02X", mode);
    return true;
  }
  if (request.rfind("SAT ", 0) == 0) {
    // "SAT <16 hex digits>": one consigne exchange in the name of the zone 1 satellite (8 data bytes).
    uint8_t data[8];
    if (request.size() != 20 || !parse_hex(request.c_str() + 4, data, 8)) {
      ESP_LOGW(TAG, "SAT request refused: expected 'SAT' and 16 hexadecimal digits");
      return false;
    }
    if (!this->engine_.request_satellite_consigne(data)) {
      ESP_LOGW(TAG, "SAT request refused (satellite_override off, or one already pending)");
      return false;
    }
    ESP_LOGI(TAG, "SAT consigne queued: %s", format_hex_pretty(data, 8).c_str());
    return true;
  }
  unsigned address = 0, words = 0;
  if (sscanf(request.c_str(), "%x %x", &address, &words) != 2 || address > 0xFFFF) {
    ESP_LOGW(TAG, "EXPLORE request '%s' refused: expected 'ADDR WORDS' in hexadecimal", request.c_str());
    return false;
  }
  if (!this->engine_.request_explore_read(static_cast<uint16_t>(address), static_cast<uint16_t>(words))) {
    ESP_LOGW(TAG, "EXPLORE %04X+%02X refused (explore: false, words not 1..1C, or one already pending)", address,
             words);
    return false;
  }
  ESP_LOGI(TAG, "EXPLORE %04X+%02X queued", address, words);
  return true;
#else
  return false;
#endif
}

bool FrisquetRadio::start_network_recovery() {
  if (this->radio_ == nullptr || this->is_failed()) return false;
  if (!this->pairing_.start(::frisquet::PairingKind::kRecoverNetwork, ::frisquet::kIdConnect, this->active_network_,
                            millis())) {
    ESP_LOGW(TAG, "NetworkID recovery refused (a pairing session is already running)");
    return false;
  }
  this->pairing_outdoor_ = false;
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
  this->pairing_.set_confirm_payload(this->pairing_version_set_ ? this->pairing_version_ : nullptr);
  this->pairing_outdoor_ = false;
  if (this->pairing_version_set_)
    ESP_LOGI(TAG, "Association: answering with version %02X %02X %02X %02X", this->pairing_version_[0],
             this->pairing_version_[1], this->pairing_version_[2], this->pairing_version_[3]);
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

bool FrisquetRadio::start_outdoor_association() {
#ifdef FRISQUET_TX_ENABLED
  if (!this->outdoor_emulation_ || !this->pairing_allowed_ || this->killed_ || this->dry_run_ ||
      this->tx_level_ < 3 || !this->on_installation_network_() || this->association_ == 0xFF ||
      !this->engine_.outdoor_sample().usable(millis()) || this->engine_.outdoor_sample().stale(millis()) ||
      this->is_failed() || this->radio_ == nullptr) {
    ESP_LOGW(TAG, "Outdoor association refused: enable outdoor_emulation, pairing and writes, pair the Connect, and supply a valid temperature first");
    return false;
  }
  if (!this->pairing_.start(::frisquet::PairingKind::kAssociate, ::frisquet::kIdOutdoorSensor,
                             this->active_network_, millis())) {
    ESP_LOGW(TAG, "Outdoor association refused: another pairing session is active");
    return false;
  }
  static const uint8_t version[4] = {0x01, 0x21, 0x01, 0x02};
  this->pairing_.set_confirm_payload(version);
  this->pairing_outdoor_ = true;
  this->pending_tx_valid_ = false;
  this->apply_network_();
  ESP_LOGW(TAG, "Outdoor sensor pairing started explicitly; this changes boiler regulation");
  return true;
#else
  ESP_LOGW(TAG, "Outdoor association requires tx: write");
  return false;
#endif
}

void FrisquetRadio::receive_outdoor_temperature(float celsius, int32_t age_seconds) {
#ifdef FRISQUET_TX_ENABLED
  if (!this->engine_.outdoor_sample().update(celsius, age_seconds, millis())) {
    ESP_LOGW(TAG, "Outdoor sample rejected: temperature must be -40..60 C, age 0..10799 seconds");
    return;
  }
  this->sync_outdoor_(millis());
#else
  ESP_LOGW(TAG, "Outdoor sample ignored: transmit path is disabled");
#endif
}

void FrisquetRadio::sync_outdoor_(uint32_t now) {
#ifdef FRISQUET_TX_ENABLED
  auto &sample = this->engine_.outdoor_sample();
  sample.tick(now);
  const char *state = !this->outdoor_emulation_ ? "Désactivée"
      : !sample.usable(now) ? "ALERTE : mesure absente ou expirée, émission arrêtée"
      : this->outdoor_association_ == 0xFF ? "Non associée"
      : this->killed_ ? "Arrêt d'urgence"
      : sample.stale(now) ? "ALERTE : ancienne mesure conservée (maximum 180 min)"
      : this->engine_.outdoor_confirmed() && now - this->engine_.outdoor_last_ok_ms() < 15UL * 60UL * 1000UL
          ? "Mesure fraîche, chaudière répond"
          : "Mesure fraîche, attente de réponse chaudière";
  if (this->last_outdoor_state_ != state) {
    this->last_outdoor_state_ = state;
    if (sample.stale(now) && this->outdoor_emulation_) ESP_LOGW(TAG, "Outdoor sensor: %s", state);
    this->publish_text_(kOutdoorEmulationState, state);
  }
#else
  (void)now;
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
#ifdef FRISQUET_TX_ENABLED
  if (!this->outdoor_pref_.save(&blob) || !global_preferences->sync())
    ESP_LOGE(TAG, "Clearing outdoor sensor association failed");
#endif
  this->outdoor_association_ = 0xFF;
#ifdef FRISQUET_TX_ENABLED
  {
    auto policy = this->engine_.policy();
    policy.outdoor_association = 0xFF;
    this->engine_.set_policy(policy);
  }
#endif
  memcpy(this->active_network_, this->network_id_, 4);
  this->network_learned_ = false;
#ifdef FRISQUET_TX_ENABLED
  if (this->association_ != this->yaml_association_) {
    this->association_ = this->yaml_association_;
    ::frisquet::TxPolicy policy = this->engine_.policy();
    policy.association = this->association_;
    policy.outdoor_association = 0xFF;
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
  if (!::frisquet::is_usable_network(this->active_network_)) {
    ESP_LOGW(TAG, "Not paired yet: put the boiler in pairing mode for a Connect and press « Radio associer le Connect »");
    this->publish_text_(kNetwork, "non associé");
    return;
  }
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
      policy.pairing_version = this->pairing_version_set_;
      memcpy(policy.pairing_payload, this->pairing_version_, 4);
      if (this->pairing_outdoor_) {
        policy.self_id = ::frisquet::kIdOutdoorSensor;
        policy.pairing_version = true;
        const uint8_t version[4] = {0x01, 0x21, 0x01, 0x02};
        memcpy(policy.pairing_payload, version, 4);
        if (!::frisquet::outdoor_offer_allowed(this->pairing_.offer(), this->pairing_.offer_length(),
                                               this->active_network_) ||
            !this->engine_.outdoor_sample().usable(now) || this->killed_) {
          ESP_LOGW(TAG, "Outdoor pairing refused: wrong network/device, stale sample or emergency stop");
          this->pairing_.cancel();
          this->pairing_.acknowledge();
          length = 0;
        }
      }
      if (length == 0) {
        this->apply_network_();
        return;
      }
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
        this->pairing_.acknowledge();
      }
#else
      this->pairing_.cancel();
#endif
    }
  }

  const ::frisquet::PairingState state = this->pairing_.state();
  if (state == ::frisquet::PairingState::kSucceeded) {
    if (this->pairing_outdoor_) {
#ifdef FRISQUET_TX_ENABLED
      this->outdoor_association_ = this->pairing_.association();
      NetworkStoreBlob blob;
      ::frisquet::pack_network_store(this->active_network_, this->outdoor_association_, blob.data);
      if (!this->outdoor_pref_.save(&blob) || !global_preferences->sync())
        ESP_LOGE(TAG, "Saving outdoor sensor association failed");
      auto policy = this->engine_.policy();
      policy.outdoor_association = this->outdoor_association_;
      this->engine_.set_policy(policy);
      ESP_LOGI(TAG, "Outdoor sensor associated; Connect identity unchanged");
#endif
    } else {
      if (memcmp(this->active_network_, this->pairing_.network(), 4) != 0) {
        this->outdoor_association_ = 0xFF;
#ifdef FRISQUET_TX_ENABLED
        auto policy = this->engine_.policy();
        policy.outdoor_association = 0xFF;
        this->engine_.set_policy(policy);
#endif
      }
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
    this->pairing_outdoor_ = false;
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
  if (this->rx_boosted_gain_) {
    state = this->radio_->setRxBoostedGainMode(true);
    this->rx_boosted_result_ = state;
    if (state != RADIOLIB_ERR_NONE) ESP_LOGW(TAG, "setRxBoostedGainMode failed: %d", state);
    else ESP_LOGI(TAG, "SX1262 boosted receive gain on");
  }
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
  this->publish_text_(kLastAlert, "Aucune");

#ifdef FRISQUET_TX_ENABLED
  ::frisquet::TxPolicy policy;
  this->outdoor_pref_ = global_preferences->make_preference<NetworkStoreBlob>(fnv1_hash("frisquet_outdoor"));
  {
    NetworkStoreBlob blob;
    uint8_t network[4], association;
    if (this->outdoor_pref_.load(&blob) &&
        ::frisquet::unpack_network_store(blob.data, sizeof(blob.data), network, association) &&
        memcmp(network, this->active_network_, 4) == 0)
      this->outdoor_association_ = association;
  }
  policy.outdoor_enabled = this->outdoor_emulation_;
  policy.outdoor_association = this->outdoor_association_;
  policy.level = this->tx_level_ >= 3   ? ::frisquet::TxLevel::kWrite
                 : this->tx_level_ >= 2 ? ::frisquet::TxLevel::kAck
                                        : ::frisquet::TxLevel::kRead;
  policy.dry_run = this->dry_run_;
  policy.kill_switch = this->killed_;
  policy.self_id = ::frisquet::kIdConnect;
  policy.association = this->association_;
  policy.explore_reads = this->explore_allowed_;
  policy.live_body_reads = this->live_body_reads_;
  policy.satellite_override = this->satellite_override_;
  policy.satellite_association = this->satellite_association_;
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
  if (now - this->last_outdoor_publish_ms_ >= 1000) {
    this->last_outdoor_publish_ms_ = now;
    this->sync_outdoor_(now);
  }
#endif
#ifdef USE_CLIMATE
  if (this->zone_climate_ != nullptr) this->zone_climate_->refresh();
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
    case ::frisquet::TxClass::kPushAck: return "push-ack";
    case ::frisquet::TxClass::kHotWaterWrite: return "hot-water-write";
    case ::frisquet::TxClass::kHolidayWrite: return "holiday-write";
    case ::frisquet::TxClass::kOutdoorWrite: return "outdoor-temperature";
    case ::frisquet::TxClass::kZoneWrite: return "zone-write";
    case ::frisquet::TxClass::kSatelliteConsigne: return "satellite-consigne";
    case ::frisquet::TxClass::kSatelliteBlock: return "satellite-block";
    case ::frisquet::TxClass::kAssociationConfirm: return "association-confirm";
    default: return "?";
  }
}

void FrisquetRadio::tx_step_(uint32_t now) {
  this->sync_hot_water_();
  this->sync_holiday_();
  this->sync_zone_();
  if (this->engine_.take_explore_failed())
    ESP_LOGI(TAG, "EXPLORE %04X+%02X: %s", this->engine_.explore_address(), this->engine_.explore_words(),
             this->engine_.explore_refused() ? "boiler refused" : "no answer");
  // On the broadcast network (or until the installation's sync word is applied again) nothing of ours goes out.
  if (!this->on_installation_network_() || this->association_ == 0xFF) {
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
  if (this->pending_tx_.kind == ::frisquet::TxClass::kOutdoorWrite &&
      !this->engine_.outdoor_tx_allowed(millis())) {
    ESP_LOGW(TAG, "Outdoor transmission cancelled: disabled, unpaired or expired temperature");
    return;
  }
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

void FrisquetRadio::update_boiler_state_(uint8_t raw) {
  this->boiler_state_ = ::frisquet::decode_boiler_state(raw);
  this->state_known_ = true;
  const int fault = this->boiler_state_.fault || this->boiler_state_.anomaly ? 1 : 0;
  if (fault == this->last_fault_) return;
  if (fault) {
    char text[48];
    snprintf(text, sizeof(text), "%s (état %02X)",
             this->boiler_state_.anomaly ? "Anomalie chaudière" : "Anomalie de liaison radio", raw);
    ESP_LOGW(TAG, "Boiler state: %s", text);
    this->publish_text_(kLastAlert, text);
  } else if (this->last_fault_ == 1) {
    ESP_LOGI(TAG, "Boiler state: anomaly cleared (état %02X)", raw);
  }
  this->last_fault_ = fault;
  if (this->fault_sensor_ != nullptr) this->fault_sensor_->publish_state(fault);
}

// Alerts: a device's text alert (43) or a boiler event (45). True when the frame was one of them.
bool FrisquetRadio::dispatch_alert_(const uint8_t *payload, size_t length) {
  char text[80];
  uint8_t source = 0;
  if (::frisquet::decode_text_alert(payload, length, source, text, sizeof(text))) {
    ESP_LOGW(TAG, "Alert from %02X: '%s'", source, text);
    this->publish_text_(kLastAlert, text[0] ? text : "Aucune");
    return true;
  }
  ::frisquet::BoilerEvent event;
  if (::frisquet::decode_boiler_event(payload, length, event)) {
    // The boiler's clock is local time stored as UTC (EXTERNAL-RESEARCH): print it as such.
    const time_t t = event.time;
    struct tm when;
    gmtime_r(&t, &when);
    char line[64];
    snprintf(line, sizeof(line), "Événement appareil %02X code %02X (%02d/%02d %02d:%02d)", event.device, event.code,
             when.tm_mday, when.tm_mon + 1, when.tm_hour, when.tm_min);
    ESP_LOGW(TAG, "Boiler event: %s", line);
    this->publish_text_(kLastAlert, line);
    return true;
  }
  return false;
}

void FrisquetRadio::dispatch_(const uint8_t *payload, size_t length) {
  if (this->dispatch_alert_(payload, length)) return;
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
    case ::frisquet::ReadKind::kHoliday: {
      ::frisquet::HolidayBlock holiday;
      if (!::frisquet::decode_holiday_block(frame, length, holiday)) {
        ESP_LOGW(TAG, "Invalid holiday block: %s", format_hex_pretty(frame, length).c_str());
        this->publish_text_(kHolidayState, "Bloc vacances non pris en charge");
        break;
      }
      if (!holiday.valid) {
        ESP_LOGW(TAG, "Invalid holiday period: start=%lu end=%lu", static_cast<unsigned long>(holiday.start),
                 static_cast<unsigned long>(holiday.end));
        this->publish_text_(kHolidayState, "Période invalide ; annulez les vacances");
        break;
      }
      if (holiday.start == 0 && holiday.end == 0) {
        this->publish_text_(kHolidayState, "Aucune");
        break;
      }
      const time_t start = static_cast<time_t>(holiday.start);
      const time_t end = static_cast<time_t>(holiday.end);
      struct tm start_tm {};
      struct tm end_tm {};
      if (gmtime_r(&start, &start_tm) == nullptr || gmtime_r(&end, &end_tm) == nullptr) {
        ESP_LOGW(TAG, "Could not format holiday timestamps");
        this->publish_text_(kHolidayState, "Dates illisibles");
        break;
      }
      char start_text[17], end_text[17];
      if (strftime(start_text, sizeof(start_text), "%Y-%m-%d %H:%M", &start_tm) == 0 ||
          strftime(end_text, sizeof(end_text), "%Y-%m-%d %H:%M", &end_tm) == 0) {
        ESP_LOGW(TAG, "Could not format holiday date strings");
        this->publish_text_(kHolidayState, "Dates illisibles");
        break;
      }
      char text[48];
      snprintf(text, sizeof(text), "%s → %s", start_text, end_text);
      this->publish_text_(kHolidayState, text);
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
    case ::frisquet::ReadKind::kExplore: {
#ifdef FRISQUET_TX_ENABLED
      // Header (6), length byte, data: only the data is logged.
      const size_t data = length > 7 ? length - 7 : 0;
      ESP_LOGI(TAG, "EXPLORE %04X+%02X: %s", this->engine_.explore_address(), this->engine_.explore_words(),
               format_hex(frame + 7, data).c_str());
#endif
      break;
    }
    case ::frisquet::ReadKind::kBody: {
      float celsius = NAN;
      if (::frisquet::decode_body_live(frame, length, celsius)) {
        this->publish_(kHeatingBodyLive, celsius);
        this->burner_.feed_body(millis(), celsius);
        this->publish_burner_();
      }
      break;
    }
    case ::frisquet::ReadKind::kFlue: {
      float celsius = NAN;
      if (::frisquet::decode_flue_snapshot(frame, length, celsius)) {
        this->burner_.feed_flue(celsius);
        this->publish_burner_();
      }
      break;
    }
    case ::frisquet::ReadKind::kSatelliteConsigne:
      ESP_LOGI(TAG, "SAT consigne answered by the boiler: %s", format_hex_pretty(frame, length).c_str());
      break;
    case ::frisquet::ReadKind::kPanel: {
      ::frisquet::PanelDiag diag;
      if (::frisquet::decode_panel_diag(frame, length, diag)) {
        char text[12];
        snprintf(text, sizeof(text), "V%X.%02X", diag.version >> 8, diag.version & 0xFF);
        this->publish_text_(kPanelVersion, text);
        this->publish_(kZone1FlowMin, diag.flow_min);
        this->publish_(kZone1FlowMax, diag.flow_max);
      }
      break;
    }
    case ::frisquet::ReadKind::kPaired: {
      char text[96];
      if (::frisquet::format_paired_versions(frame, length, text, sizeof(text))) this->publish_text_(kPairedVersions, text);
      break;
    }
    case ::frisquet::ReadKind::kSatelliteBlock:
      ESP_LOGI(TAG, "SATZ block answered by the boiler: %s", format_hex_pretty(frame, length).c_str());
      break;
    case ::frisquet::ReadKind::kStatus: {
      ::frisquet::SatelliteBroadcast status;
      if (::frisquet::decode_status_read(frame, length, status)) {
        // Exploration (29/10): is this state byte live, unlike the 10-minute broadcast?
        ESP_LOGI(TAG, "Status A029: state=%02X mode=%02X clock %02X:%02X:%02X", status.state_raw, status.mode_raw[0],
                 status.hour, status.minute, status.second);
        this->update_boiler_state_(status.state_raw);
        this->burner_.set_heating_demand(this->boiler_state_.running);
        this->publish_burner_();
        this->publish_text_(kBoilerState, this->boiler_state_.fault ? "Anomalie"
                                          : (this->boiler_state_.running ? "En marche" : "Veille"));
        this->publish_activity_();
      }
      break;
    }
    default: break;
  }
}

void FrisquetRadio::publish_burner_() {
  const ::frisquet::BurnerState state = this->burner_.state();
  if (this->burner_sensor_ == nullptr || state == ::frisquet::BurnerState::kUnknown) return;
  const int on = state == ::frisquet::BurnerState::kOn ? 1 : 0;
  if (on == this->last_burner_) return;
  this->last_burner_ = on;
  this->burner_sensor_->publish_state(on == 1);
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
  this->zone1_ambient_ = b.ambient[0];
  this->zone1_setpoint_ = b.setpoint[0];
  this->publish_(kOutdoor, b.outdoor);
  this->publish_text_(kZone1Mode, zone_mode_name(::frisquet::decode_zone_mode(b.mode_raw[0]).mode));

  this->update_boiler_state_(b.state_raw);
  this->burner_.set_heating_demand(this->boiler_state_.running);
  this->publish_burner_();
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
  this->zone1_ambient_ = info.ambient[0];
  this->zone1_setpoint_ = info.setpoint[0];
  this->publish_(kHotWater, info.hot_water);
  this->publish_(kHeatingBody, info.heating_body);
  this->publish_(kOutdoor, info.outdoor);
  this->publish_(kFlue, info.flue);
  this->burner_.feed_flue(info.flue);
  this->publish_burner_();
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
  // Braces everywhere: below the CONFIG log level these macros expand to nothing.
  if (!::frisquet::is_usable_network(this->active_network_)) {
    ESP_LOGCONFIG(TAG, "  Network ID: not paired yet (pairing: « Radio associer le Connect »)");
  } else {
    ESP_LOGCONFIG(TAG, "  Network ID: %s (%s)", format_network(this->active_network_).c_str(),
                  this->network_learned_ ? "learned by pairing" : "YAML");
  }
  if (this->network_learned_ && ::frisquet::is_usable_network(this->network_id_)) {
    ESP_LOGCONFIG(TAG, "  YAML network ID: %s", format_network(this->network_id_).c_str());
  }
  ESP_LOGCONFIG(TAG, "  Association confirmations: %s", this->pairing_allowed_ ? "allowed" : "refused");
  ESP_LOGCONFIG(TAG, "  RX bandwidth: %.1f kHz", this->rx_bandwidth_);
  if (this->rx_boosted_result_ == 1) {
    ESP_LOGCONFIG(TAG, "  RX boosted gain: off");
  } else {
    ESP_LOGCONFIG(TAG, "  RX boosted gain: %s", this->rx_boosted_result_ == RADIOLIB_ERR_NONE ? "on" : "FAILED");
  }
  if (this->fem_) {
    ESP_LOGCONFIG(TAG, "  KCT8103L FEM: LNA bypass %s", this->lna_bypass_ ? "on" : "off");
  }
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
