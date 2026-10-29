#pragma once

#include <RadioLib.h>

#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_TEXT
#include "esphome/components/text/text.h"
#endif

#include "proto_crc.h"
#include "proto_decode.h"
#include "proto_encode.h"
#include "proto_pairing.h"
#include "proto_tracker.h"
#include "proto_zone_store.h"
#ifdef FRISQUET_TX_ENABLED
#include "proto_engine.h"
#endif

namespace esphome {
namespace frisquet_radio {

// SX1262 that exposes the raw receive buffer: the CRC is checked in software (the boiler's
// frames suffer from bit slips that the chip's own CRC would reject).
class RawRadio : public SX1262 {
 public:
  using SX1262::SX1262;
  int16_t arm();
  int16_t read_raw(uint8_t *raw, size_t &length, uint16_t &irq);
#ifdef FRISQUET_TX_ENABLED
  // Restores the on-air format (variable length, hardware CRC) and transmits one frame.
  int16_t transmit_frame(const uint8_t *data, size_t length);
#endif
};

// Order must match SENSORS / TEXT_SENSORS in __init__.py.
enum SensorId : uint8_t {
  kZone1Ambient,
  kZone1Setpoint,
  kRssi,
  kFramesValid,
  kFramesRepaired,
  kFramesRejected,
  kSecondsSinceFrame,
  kHotWater,
  kHeatingBody,
  kOutdoor,
  kFlue,
  kZone1Flow,
  kZone1FlowSetpoint,
  kPressure,
  kConsumptionHotWater,
  kConsumptionHeating,
  kAnnualHotWater,
  kAnnualHeating,
  kAcksSent,
  kReadsOk,
  kReadsFailed,
  kTxDryRuns,
  kTxBlocked,
  kSensorCount,
};

enum TextId : uint8_t {
  kZone1Mode,
  kBoilerState,
  kBoilerClock,
  kBoilerActivity,
  kHotWaterMode,
  kZone1Raw,
  kZoneBlock,
  kHotWaterWrite,
  kZoneWrite,
  kZoneProgram,
  kZoneBlockState,
  kNetwork,
  kPairingState,
  kTextCount,
};

// The persisted record: opaque bytes, packed and checked by pack_zone_store() / unpack_zone_store().
struct ZoneStoreBlob {
  uint8_t data[::frisquet::kZoneStoreSize];
};
struct NetworkStoreBlob {
  uint8_t data[::frisquet::kNetworkStoreSize];
};

// The transmit path (tx: read | ack) is compiled only when FRISQUET_TX_ENABLED is defined.
class FrisquetRadio : public Component {
 public:
  void set_pins(uint8_t cs, uint8_t irq, uint8_t reset, uint8_t busy, uint8_t sck, uint8_t miso, uint8_t mosi) {
    cs_ = cs; irq_ = irq; reset_ = reset; busy_ = busy; sck_ = sck; miso_ = miso; mosi_ = mosi;
  }
  void set_fem_heltec_v4_3(bool value) { fem_ = value; }
  void set_network_id(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    network_id_[0] = a; network_id_[1] = b; network_id_[2] = c; network_id_[3] = d;
  }
  void set_rx_bandwidth(float value) { rx_bandwidth_ = value; }
  void set_lna_bypass(bool value) { lna_bypass_ = value; }
  void set_sensor(uint8_t id, sensor::Sensor *s) { if (id < kSensorCount) sensors_[id] = s; }
  void set_text_sensor(uint8_t id, text_sensor::TextSensor *s) { if (id < kTextCount) texts_[id] = s; }
  void set_burner_sensor(binary_sensor::BinarySensor *s) { burner_sensor_ = s; }

  // tx_level: 1 = reads, 2 = reads and zone-relay acknowledgements.
  void set_tx(uint8_t level, bool dry_run, uint8_t association) {
    tx_level_ = level;
    dry_run_ = dry_run;
    association_ = association;
    yaml_association_ = association;
  }
  // YAML `pairing: true`: association confirmations may be transmitted.
  void set_pairing_allowed(bool value) { pairing_allowed_ = value; }
  // Pairing on the broadcast network (30 s, the boiler must be in pairing mode). False when it cannot start.
  // Recovery only listens and changes the NetworkID; association also confirms and changes the association byte.
  bool start_network_recovery();
  bool start_association();
  // Back to the YAML NetworkID and association, now and after reboots.
  bool forget_network();

  // Emergency stop: nothing is transmitted while set. Safe to call at any time.
  void set_kill_switch(bool killed);
  bool kill_switch() const { return killed_; }

  // Hot-water (ECS) mode: `name` is one of the select's options. False when it cannot be queued.
  bool request_hot_water_mode(const std::string &name);
  void republish_hot_water_mode();
#ifdef USE_SELECT
  void set_hot_water_select(select::Select *s) { hot_water_select_ = s; }
  void set_zone_mode_select(select::Select *s) { zone_mode_select_ = s; }
#endif
#ifdef USE_TEXT
  void set_program_text(text::Text *t) { program_text_ = t; }
#endif

  // Zone 1 mode ("Auto", "Confort", "Réduit", "Hors Gel") and weekly programme (84 hexadecimal characters).
  bool request_zone_mode(const std::string &name);
  bool request_zone_program(const std::string &hex);
  void republish_zone_block();
  void republish_zone_setpoints() { this->republish_zone_block(); }

  // Zone 1 setpoints: field 0 comfort, 1 reduced, 2 frost. False when the change cannot be queued.
  bool request_zone_setpoint(uint8_t field, float celsius);
#ifdef USE_NUMBER
  void set_zone_number(uint8_t field, number::Number *n) {
    if (field < 3) zone_numbers_[field] = n;
  }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void handle_packet_();
  void dispatch_(const uint8_t *payload, size_t length);
  void publish_read_(::frisquet::ReadKind kind, const uint8_t *frame, size_t length);
  void publish_counters_();
  void publish_satellite_broadcast_(const ::frisquet::SatelliteBroadcast &b);
  void publish_info_(const ::frisquet::BoilerInfo &info);
  void publish_activity_();
  void publish_(SensorId id, float value);
  void publish_text_(TextId id, const char *value);
  void rearm_(const char *reason);
  void pairing_step_(uint32_t now);
  void apply_network_();
  void publish_network_();
  void save_network_(uint8_t association);
  // True only when the radio really uses the installation's sync word and no pairing session runs.
  bool on_installation_network_() const {
    return !pairing_.active() && memcmp(applied_network_, active_network_, 4) == 0;
  }
#ifdef FRISQUET_TX_ENABLED
  void tx_step_(uint32_t now);
  void transmit_pending_();
  void sync_hot_water_();
  void sync_zone_();
  void restore_zone_blocks_();
  void save_zone_blocks_();
#endif

  uint8_t cs_{8}, irq_{14}, reset_{12}, busy_{13}, sck_{9}, miso_{11}, mosi_{10};
  bool fem_{false};
  uint8_t network_id_[4]{0, 0, 0, 0};       // from the YAML
  uint8_t active_network_[4]{0, 0, 0, 0};   // the installation's network: YAML, or learned by pairing
  uint8_t applied_network_[4]{0, 0, 0, 0};  // the sync word the radio currently uses
  bool network_learned_{false};
  uint8_t yaml_association_{0xFF};
  bool pairing_allowed_{false};
  ::frisquet::PairingSession pairing_;
  int last_pairing_state_{-1};
  ESPPreferenceObject network_pref_;
  float rx_bandwidth_{234.3f};
  bool lna_bypass_{false};
  uint8_t tx_level_{0};
  bool dry_run_{true};
  uint8_t association_{0xFF};
  bool killed_{false};

  RawRadio *radio_{nullptr};
  uint32_t last_arm_ms_{0};
  uint32_t last_frame_ms_{0};
  uint32_t last_age_publish_ms_{0};
  bool frame_seen_{false};
  std::string last_zone_block_;
  uint32_t valid_{0}, repaired_{0}, rejected_{0}, ignored_{0};

  ::frisquet::ReadTracker tracker_;
  bool state_known_{false};
  ::frisquet::BoilerState boiler_state_{};
  ::frisquet::OperatingMode operating_mode_{::frisquet::OperatingMode::kUnknown};

#ifdef FRISQUET_TX_ENABLED
  ::frisquet::ConnectEngine engine_;
  ::frisquet::TxRequest pending_tx_;
  bool pending_tx_valid_{false};
  uint32_t pending_tx_due_ms_{0};
  uint32_t last_stats_ms_{0};
  uint32_t tx_errors_{0};
  ::frisquet::WriteState last_write_state_{::frisquet::WriteState::kIdle};
  int last_published_mode_{-1};
  ::frisquet::WriteState last_zone_state_{::frisquet::WriteState::kIdle};
  int last_zone_raw_[3]{-1, -1, -1};
  int last_zone_mode_{-1};
  int broadcast_zone_mode_{0};  // zone block mode (0x05..0x08) matching the last broadcast, 0 if unknown
  int last_block_state_{-1};
  ESPPreferenceObject store_pref_;
  std::string last_program_;
#endif
#ifdef USE_NUMBER
  number::Number *zone_numbers_[3]{};
#endif
#ifdef USE_TEXT
  text::Text *program_text_{nullptr};
#endif
#ifdef USE_SELECT
  select::Select *hot_water_select_{nullptr};
  select::Select *zone_mode_select_{nullptr};
#endif

  sensor::Sensor *sensors_[kSensorCount]{};
  text_sensor::TextSensor *texts_[kTextCount]{};
  binary_sensor::BinarySensor *burner_sensor_{nullptr};
};

}  // namespace frisquet_radio
}  // namespace esphome
