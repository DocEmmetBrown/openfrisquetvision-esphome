#pragma once

#include <RadioLib.h>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include "proto_crc.h"
#include "proto_decode.h"

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

// Phase 0/1: receive only. There is intentionally no transmit method in this class.
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

  void set_zone1_ambient_temperature_sensor(sensor::Sensor *s) { zone1_ambient_ = s; }
  void set_zone1_setpoint_sensor(sensor::Sensor *s) { zone1_setpoint_ = s; }
  void set_rssi_sensor(sensor::Sensor *s) { rssi_ = s; }
  void set_frames_valid_sensor(sensor::Sensor *s) { frames_valid_ = s; }
  void set_frames_repaired_sensor(sensor::Sensor *s) { frames_repaired_ = s; }
  void set_frames_rejected_sensor(sensor::Sensor *s) { frames_rejected_ = s; }
  void set_zone1_mode_sensor(text_sensor::TextSensor *s) { zone1_mode_ = s; }
  void set_boiler_state_sensor(text_sensor::TextSensor *s) { boiler_state_ = s; }
  void set_boiler_clock_sensor(text_sensor::TextSensor *s) { boiler_clock_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void handle_packet_();
  void publish_counters_();
  void publish_satellite_broadcast_(const ::frisquet::SatelliteBroadcast &b);
  void rearm_(const char *reason);

  uint8_t cs_{8}, irq_{14}, reset_{12}, busy_{13}, sck_{9}, miso_{11}, mosi_{10};
  bool fem_{false};
  uint8_t network_id_[4]{0, 0, 0, 0};
  float rx_bandwidth_{234.3f};
  bool lna_bypass_{false};

  RawRadio *radio_{nullptr};
  uint32_t last_arm_ms_{0};
  uint32_t last_frame_ms_{0};
  uint32_t valid_{0}, repaired_{0}, rejected_{0}, ignored_{0};

  sensor::Sensor *zone1_ambient_{nullptr};
  sensor::Sensor *zone1_setpoint_{nullptr};
  sensor::Sensor *rssi_{nullptr};
  sensor::Sensor *frames_valid_{nullptr};
  sensor::Sensor *frames_repaired_{nullptr};
  sensor::Sensor *frames_rejected_{nullptr};
  text_sensor::TextSensor *zone1_mode_{nullptr};
  text_sensor::TextSensor *boiler_state_{nullptr};
  text_sensor::TextSensor *boiler_clock_{nullptr};
};

}  // namespace frisquet_radio
}  // namespace esphome
