#include "frisquet_radio.h"

#include <SPI.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace frisquet_radio {

static const char *const TAG = "frisquet_radio";

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
  if (g_rx_flag) {
    g_rx_flag = false;
    this->handle_packet_();
    this->rearm_("after packet");
  } else if (millis() - this->last_arm_ms_ > 30000) {
    // Safety net against a lost interrupt or a chip that left RX.
    this->rearm_("watchdog");
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
  this->last_frame_ms_ = millis();

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
  if (this->rssi_ != nullptr) this->rssi_->publish_state(rssi);

  ::frisquet::Header header;
  ::frisquet::parse_header(payload, result.length, header);
  ESP_LOGD(TAG, "RX %u bytes %02X->%02X type=%02X rssi=%.1f %s", static_cast<unsigned>(result.length),
           header.source, header.destination, header.type, rssi, format_hex_pretty(payload, result.length).c_str());

  ::frisquet::SatelliteBroadcast broadcast;
  if (::frisquet::decode_satellite_broadcast(payload, result.length, broadcast)) {
    this->publish_satellite_broadcast_(broadcast);
  } else {
    this->ignored_++;
  }
  this->publish_counters_();
}

void FrisquetRadio::publish_counters_() {
  if (this->frames_valid_ != nullptr) this->frames_valid_->publish_state(this->valid_);
  if (this->frames_repaired_ != nullptr) this->frames_repaired_->publish_state(this->repaired_);
  if (this->frames_rejected_ != nullptr) this->frames_rejected_->publish_state(this->rejected_);
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
  if (this->zone1_ambient_ != nullptr && !std::isnan(b.ambient[0])) this->zone1_ambient_->publish_state(b.ambient[0]);
  if (this->zone1_setpoint_ != nullptr && !std::isnan(b.setpoint[0])) this->zone1_setpoint_->publish_state(b.setpoint[0]);
  if (this->zone1_mode_ != nullptr) {
    this->zone1_mode_->publish_state(zone_mode_name(::frisquet::decode_zone_mode(b.mode_raw[0]).mode));
  }
  if (this->boiler_state_ != nullptr) {
    const ::frisquet::BoilerState state = ::frisquet::decode_boiler_state(b.state_raw);
    this->boiler_state_->publish_state(state.fault ? "Anomalie" : (state.running ? "En marche" : "Veille"));
  }
  if (this->boiler_clock_ != nullptr) {
    // The date is sent as BCD: 0x26 0x10 0x25 means 2026-10-25.
    char text[24];
    snprintf(text, sizeof(text), "20%02X-%02X-%02X %02X:%02X:%02X", b.year, b.month, b.day, b.hour, b.minute,
             b.second);
    this->boiler_clock_->publish_state(text);
  }
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
