// Host tests for the pure C++ core, replaying frames captured on a real boiler.
// Build and run: tests/host/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../components/frisquet/proto_crc.h"
#include "../../components/frisquet/proto_decode.h"

#ifndef FIXTURES
#define FIXTURES "tests/fixtures/frames.txt"
#endif

using Bytes = std::vector<uint8_t>;
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) \
  do { \
    ++g_checks; \
    if (!(cond)) { \
      ++g_failures; \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabsf((a) - (b)) <= (eps))

static std::map<std::string, Bytes> load_fixtures() {
  std::map<std::string, Bytes> frames;
  std::ifstream file(FIXTURES);
  if (!file) {
    printf("cannot open %s (run from the repository root)\n", FIXTURES);
    exit(2);
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string name = line.substr(0, eq);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::istringstream bytes(line.substr(eq + 1));
    std::string token;
    Bytes data;
    while (bytes >> token) data.push_back(static_cast<uint8_t>(strtoul(token.c_str(), nullptr, 16)));
    frames[name] = data;
  }
  return frames;
}

static std::map<std::string, Bytes> F;

static void test_header() {
  frisquet::Header h;
  const Bytes &req = F["payload_79f2_request"];
  CHECK(frisquet::parse_header(req.data(), req.size(), h));
  CHECK(h.destination == frisquet::kIdBoiler);
  CHECK(h.source == frisquet::kIdConnect);
  CHECK(h.association == 0xE3);
  CHECK(h.type == frisquet::kRead);
  CHECK(!h.is_ack());
  const Bytes &rsp = F["payload_79f2_idle"];
  CHECK(frisquet::parse_header(rsp.data(), rsp.size(), h));
  CHECK(h.is_ack());
  CHECK(!frisquet::parse_header(rsp.data(), 5, h));
}

static void test_codecs() {
  const uint8_t t[] = {0x00, 0xBA};
  CHECK_NEAR(frisquet::temperature16(t), 18.6f, 0.001f);
  const uint8_t absent127[] = {0x04, 0xF6};
  const uint8_t absent129[] = {0x05, 0x0A};
  CHECK(isnan(frisquet::temperature16_or_nan(absent127)));
  CHECK(isnan(frisquet::temperature16_or_nan(absent129)));
  const uint8_t negative[] = {0xFF, 0xCE};  // -5.0
  CHECK_NEAR(frisquet::temperature16(negative), -5.0f, 0.001f);
  const uint8_t p[] = {0x1E, 0x00};
  CHECK_NEAR(frisquet::pressure16(p), 1.5f, 0.0001f);
}

static void test_boiler_info_idle() {
  const Bytes &f = F["payload_79f2_idle"];
  CHECK(f.size() == 63);
  frisquet::BoilerInfo info;
  CHECK(frisquet::decode_boiler_info(f.data(), f.size(), info));
  CHECK_NEAR(info.ambient[0], 18.6f, 0.001f);
  CHECK(isnan(info.ambient[1]) && isnan(info.ambient[2]));
  CHECK_NEAR(info.flow_setpoint[0], 32.2f, 0.001f);
  CHECK_NEAR(info.setpoint[0], 20.0f, 0.001f);
  CHECK(isnan(info.outdoor));
  CHECK_NEAR(info.hot_water, 74.2f, 0.001f);
  CHECK_NEAR(info.heating_body, 78.2f, 0.001f);
  CHECK_NEAR(info.flow[0], 28.8f, 0.001f);
  CHECK_NEAR(info.safety_probe, 32.0f, 0.001f);
  CHECK_NEAR(info.flue, 0.0f, 0.001f);
  CHECK_NEAR(info.pressure, 1.4f, 0.001f);
  CHECK(frisquet::decode_operating_mode(info.operating_mode_raw) == frisquet::OperatingMode::kRadiators);
}

static void test_boiler_info_flue_reported_while_firing() {
  const Bytes &f = F["payload_79f2_firing"];
  frisquet::BoilerInfo info;
  CHECK(frisquet::decode_boiler_info(f.data(), f.size(), info));
  // The flue sensor is populated while the burner runs: 0x0999 / 10 / 2.
  CHECK_NEAR(info.flue, 122.85f, 0.01f);
  CHECK_NEAR(info.hot_water, 64.4f, 0.001f);
  CHECK_NEAR(info.heating_body, 60.0f, 0.001f);
  CHECK_NEAR(info.flow[0], 46.5f, 0.001f);
  CHECK_NEAR(info.pressure, 1.55f, 0.001f);
  // Instant powers stay at zero on this boiler even while firing.
  CHECK_NEAR(info.hot_water_power, 0.0f, 0.001f);
  CHECK_NEAR(info.heating_power, 0.0f, 0.001f);

  const Bytes &g = F["payload_79f2_firing2"];
  CHECK(frisquet::decode_boiler_info(g.data(), g.size(), info));
  CHECK_NEAR(info.flue, 121.25f, 0.01f);
  CHECK_NEAR(info.heating_body, 74.7f, 0.001f);
}

static void test_boiler_info_rejects_wrong_frames() {
  frisquet::BoilerInfo info;
  const Bytes &req = F["payload_79f2_request"];
  CHECK(!frisquet::decode_boiler_info(req.data(), req.size(), info));
  const Bytes &sat = F["payload_sat_broadcast"];
  CHECK(!frisquet::decode_boiler_info(sat.data(), sat.size(), info));
  const Bytes &f = F["payload_79f2_idle"];
  CHECK(!frisquet::decode_boiler_info(f.data(), 50, info));
}

static void test_satellite_broadcast() {
  frisquet::SatelliteBroadcast b;
  const Bytes &f = F["payload_sat_broadcast"];
  CHECK(f.size() == frisquet::kSatelliteBroadcastLength);
  CHECK(frisquet::decode_satellite_broadcast(f.data(), f.size(), b));
  CHECK(b.year == 0x26 && b.month == 0x10 && b.day == 0x25);
  CHECK(b.hour == 0x17 && b.minute == 0x57 && b.second == 0x03);  // BCD-looking hex, as sent
  CHECK(b.weekday == 0);
  CHECK(isnan(b.outdoor));
  CHECK_NEAR(b.ambient[0], 18.6f, 0.001f);
  CHECK_NEAR(b.setpoint[0], 20.0f, 0.001f);
  CHECK(isnan(b.ambient[1]) && isnan(b.ambient[2]));
  const frisquet::DecodedZoneMode mode = frisquet::decode_zone_mode(b.mode_raw[0]);
  CHECK(mode.mode == frisquet::ZoneMode::kComfortAuto);
  CHECK(mode.connect_compat);
  const frisquet::BoilerState st = frisquet::decode_boiler_state(b.state_raw);
  CHECK(st.running && !st.fault && !st.heating_stopped);

  const Bytes &plain = F["payload_sat_broadcast_plain"];
  CHECK(frisquet::decode_satellite_broadcast(plain.data(), plain.size(), b));
  CHECK_NEAR(b.ambient[0], 20.0f, 0.001f);
  CHECK_NEAR(b.setpoint[0], 19.0f, 0.001f);
  const frisquet::DecodedZoneMode plain_mode = frisquet::decode_zone_mode(b.mode_raw[0]);
  CHECK(plain_mode.mode == frisquet::ZoneMode::kComfortAuto);
  CHECK(!plain_mode.connect_compat);
}

static void test_zone_mode() {
  CHECK(frisquet::decode_zone_mode(0x10).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(frisquet::decode_zone_mode(0x30).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(frisquet::decode_zone_mode(0x30).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x77).mode == frisquet::ZoneMode::kUnknown);
}

static void test_activity() {
  using namespace frisquet;
  BoilerState running;
  running.running = true;
  BoilerState standby;
  CHECK(boiler_activity(true, standby, OperatingMode::kRadiators) == Activity::kStandby);
  CHECK(boiler_activity(true, running, OperatingMode::kRadiators) == Activity::kHeating);
  CHECK(boiler_activity(true, running, OperatingMode::kHotWater) == Activity::kHotWater);
  CHECK(boiler_activity(true, running, OperatingMode::kUnknown) == Activity::kUnknown);
  CHECK(boiler_activity(false, standby, OperatingMode::kRadiators) == Activity::kHeating);
  BoilerState stopped;
  stopped.running = true;
  stopped.heating_stopped = true;
  CHECK(boiler_activity(true, stopped, OperatingMode::kRadiators) == Activity::kStandby);
}

static void test_other_reads() {
  frisquet::Consumption c;
  const Bytes &f = F["payload_7a18"];
  CHECK(frisquet::decode_consumption(f.data(), f.size(), c));
  CHECK(c.hot_water == 0 && c.heating == 0);

  frisquet::DecodedHotWaterMode m;
  const Bytes &e = F["payload_a0f0"];
  CHECK(frisquet::decode_hot_water_mode(e.data(), e.size(), m));
  CHECK(m.mode == frisquet::HotWaterMode::kEcoScheduled);
  CHECK(m.frame_bits == 0x80);
}

static void check_repair(const char *raw_name, const char *payload_name, int8_t adjustment) {
  const Bytes &raw = F[raw_name];
  const Bytes &expected = F[payload_name];
  CHECK(raw.size() == frisquet::crc::kRawLength);
  Bytes before = raw;
  uint8_t payload[frisquet::crc::kMaxPayload] = {};
  const frisquet::crc::Result r = frisquet::crc::decode(raw.data(), raw.size(), payload);
  CHECK(r.status == frisquet::crc::Status::Repaired);
  CHECK(r.length == expected.size());
  CHECK(r.adjustment == adjustment);
  CHECK(r.bit == 427);
  CHECK(r.corrected_bits == 1);
  CHECK(r.length == expected.size() && memcmp(payload, expected.data(), expected.size()) == 0);
  CHECK(raw == before);  // the capture is never modified
}

static void test_crc_repair() {
  check_repair("raw_79f2_firing_removed_zero", "payload_79f2_firing", -1);
  check_repair("raw_79f2_firing2_inserted_zero", "payload_79f2_firing2", +1);
}

static void test_crc_valid_and_invalid() {
  const Bytes &p = F["payload_79f2_idle"];
  Bytes raw(frisquet::crc::kRawLength, 0x55);
  raw[0] = static_cast<uint8_t>(p.size());
  memcpy(&raw[1], p.data(), p.size());
  const uint16_t crc = frisquet::crc::crc16(raw.data(), p.size() + 1);
  raw[p.size() + 1] = crc >> 8;
  raw[p.size() + 2] = crc & 0xFF;
  uint8_t out[frisquet::crc::kMaxPayload] = {};
  frisquet::crc::Result r = frisquet::crc::decode(raw.data(), raw.size(), out);
  CHECK(r.status == frisquet::crc::Status::Valid);
  CHECK(memcmp(out, p.data(), p.size()) == 0);

  // A flipped data bit cannot be repaired (only zero-run slips are).
  Bytes bad = raw;
  bad[10] ^= 0x04;
  uint8_t untouched[frisquet::crc::kMaxPayload];
  memset(untouched, 0xAA, sizeof(untouched));
  r = frisquet::crc::decode(bad.data(), bad.size(), untouched);
  CHECK(r.status == frisquet::crc::Status::Invalid);
  CHECK(untouched[0] == 0xAA);  // payload untouched on rejection

  Bytes short_len = raw;
  short_len[0] = 3;
  CHECK(frisquet::crc::decode(short_len.data(), short_len.size(), out).status ==
        frisquet::crc::Status::InvalidLength);
  Bytes long_len = raw;
  long_len[0] = 64;
  CHECK(frisquet::crc::decode(long_len.data(), long_len.size(), out).status ==
        frisquet::crc::Status::InvalidLength);
}

int main() {
  F = load_fixtures();
  test_header();
  test_codecs();
  test_boiler_info_idle();
  test_boiler_info_flue_reported_while_firing();
  test_boiler_info_rejects_wrong_frames();
  test_satellite_broadcast();
  test_zone_mode();
  test_activity();
  test_other_reads();
  test_crc_repair();
  test_crc_valid_and_invalid();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
