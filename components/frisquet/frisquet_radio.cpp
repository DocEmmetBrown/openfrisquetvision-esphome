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

static volatile bool g_rx_flag = false;
static void IRAM_ATTR on_rx_irq() { g_rx_flag = true; }

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
      // CTX low in RX keeps the LNA in the path.
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
  state = this->radio_->setSyncWord(this->network_id_, 4);
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "setSyncWord failed: %d", state);
    this->mark_failed();
    return;
  }
  this->radio_->setPacketReceivedAction(on_rx_irq);
  this->rearm_("setup");
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
  if (this->frame_seen_ && millis() - this->last_age_publish_ms_ >= 30000) {
    const uint32_t current = millis();
    this->last_age_publish_ms_ = current;
    this->publish_(kSecondsSinceFrame, (current - this->last_frame_ms_) / 1000.0f);
  }
}

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
    return;
  }

  // Answers to a Connect's reads, overheard when a real Connect is on the air.
  ::frisquet::ReadKind kind = ::frisquet::ReadKind::kUnknown;
  if (!this->tracker_.on_frame(payload, length, millis(), kind)) {
    this->ignored_++;
    return;
  }
  switch (kind) {
    case ::frisquet::ReadKind::kInfo: {
      ::frisquet::BoilerInfo info;
      if (::frisquet::decode_boiler_info(payload, length, info)) this->publish_info_(info);
      break;
    }
    case ::frisquet::ReadKind::kConsumption: {
      ::frisquet::Consumption consumption;
      if (::frisquet::decode_consumption(payload, length, consumption)) {
        if (consumption.hot_water >= 0) this->publish_(kConsumptionHotWater, consumption.hot_water);
        if (consumption.heating >= 0) this->publish_(kConsumptionHeating, consumption.heating);
      }
      break;
    }
    case ::frisquet::ReadKind::kAnnualConsumption: {
      ::frisquet::AnnualConsumption annual;
      if (::frisquet::decode_annual_consumption(payload, length, annual)) {
        this->publish_(kAnnualHotWater, annual.hot_water);
        this->publish_(kAnnualHeating, annual.heating);
      }
      break;
    }
    case ::frisquet::ReadKind::kHotWaterMode: {
      ::frisquet::DecodedHotWaterMode mode;
      if (::frisquet::decode_hot_water_mode(payload, length, mode)) {
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
  this->publish_activity_();
}

void FrisquetRadio::publish_info_(const ::frisquet::BoilerInfo &info) {
  this->publish_(kZone1Ambient, info.ambient[0]);
  this->publish_(kZone1Setpoint, info.setpoint[0]);
  this->publish_(kHotWater, info.hot_water);
  this->publish_(kHeatingBody, info.heating_body);
  this->publish_(kOutdoor, info.outdoor);
  this->publish_(kFlue, info.flue);
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
  ESP_LOGCONFIG(TAG, "Frisquet radio (receive only):");
  ESP_LOGCONFIG(TAG, "  Pins: CS=%u IRQ=%u RST=%u BUSY=%u SCK=%u MISO=%u MOSI=%u", this->cs_, this->irq_, this->reset_,
                this->busy_, this->sck_, this->miso_, this->mosi_);
  ESP_LOGCONFIG(TAG, "  Network ID: %02X:%02X:%02X:%02X", this->network_id_[0], this->network_id_[1],
                this->network_id_[2], this->network_id_[3]);
  ESP_LOGCONFIG(TAG, "  RX bandwidth: %.1f kHz", this->rx_bandwidth_);
  if (this->fem_) ESP_LOGCONFIG(TAG, "  KCT8103L FEM: LNA bypass %s", this->lna_bypass_ ? "on" : "off");
  ESP_LOGCONFIG(TAG, "  Frames: valid=%u repaired=%u rejected=%u", this->valid_, this->repaired_, this->rejected_);
}

}  // namespace frisquet_radio
}  // namespace esphome
