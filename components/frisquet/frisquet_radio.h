#pragma once

#include <RadioLib.h>

#include <string>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include "proto_crc.h"
#include "proto_decode.h"
#include "proto_encode.h"
#include "proto_tracker.h"

namespace esphome {
namespace frisquet_radio {

// SX1262 that exposes the raw receive buffer: the CRC is checked in software (the boiler's
// frames suffer from bit slips that the chip's own CRC would reject).
class RawRadio : public SX1262 {
 public:
  using SX1262::SX1262;
  int16_t arm();
  int16_t read_raw(uint8_t *raw, size_t &length, uint16_t &irq);
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
  kTextCount,
};

// Receive only. There is intentionally no transmit method in this class.
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

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void handle_packet_();
  void dispatch_(const uint8_t *payload, size_t length);
  void publish_counters_();
  void publish_satellite_broadcast_(const ::frisquet::SatelliteBroadcast &b);
  void publish_info_(const ::frisquet::BoilerInfo &info);
  void publish_activity_();
  void publish_(SensorId id, float value);
  void publish_text_(TextId id, const char *value);
  void rearm_(const char *reason);

  uint8_t cs_{8}, irq_{14}, reset_{12}, busy_{13}, sck_{9}, miso_{11}, mosi_{10};
  bool fem_{false};
  uint8_t network_id_[4]{0, 0, 0, 0};
  float rx_bandwidth_{234.3f};
  bool lna_bypass_{false};

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

  sensor::Sensor *sensors_[kSensorCount]{};
  text_sensor::TextSensor *texts_[kTextCount]{};
};

}  // namespace frisquet_radio
}  // namespace esphome
