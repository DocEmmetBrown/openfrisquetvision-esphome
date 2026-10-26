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
#include "../../components/frisquet/proto_engine.h"
#include "../../components/frisquet/proto_schedule.h"
#include "../../components/frisquet/proto_transaction.h"
#include "../../components/frisquet/proto_txgate.h"
#include "../../components/frisquet/proto_tracker.h"

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

static void test_zone_block_and_fault_broadcast() {
  frisquet::ZoneBlock z;
  const Bytes &f = F["payload_zone_block"];
  CHECK(f.size() == frisquet::kZoneBlockLength);
  CHECK(frisquet::decode_zone_block(f.data(), f.size(), z));
  CHECK_NEAR(z.comfort, 20.0f, 0.001f);
  CHECK_NEAR(z.reduced, 17.0f, 0.001f);
  CHECK_NEAR(z.frost, 8.0f, 0.001f);
  CHECK(z.mode == 0x05 && z.options == 0x25);
  CHECK(z.source == frisquet::kIdBoiler && z.destination == frisquet::kIdConnect);
  Bytes wrong = f;
  wrong[14] = 0x2F;
  CHECK(!frisquet::decode_zone_block(wrong.data(), wrong.size(), z));
  const Bytes &broadcast = F["payload_sat_broadcast"];
  CHECK(!frisquet::decode_zone_block(broadcast.data(), broadcast.size(), z));

  frisquet::SatelliteBroadcast b;
  const Bytes &g = F["payload_sat_broadcast_fault"];
  CHECK(frisquet::decode_satellite_broadcast(g.data(), g.size(), b));
  CHECK(b.state_raw == 0x29 && b.weekday == 1);
  CHECK(b.day == 0x26 && b.hour == 0x09 && b.minute == 0x20);
  const frisquet::BoilerState st = frisquet::decode_boiler_state(b.state_raw);
  CHECK(st.fault && st.running && !st.heating_stopped);
  CHECK_NEAR(b.ambient[0], 18.9f, 0.001f);
  CHECK_NEAR(b.setpoint[0], 19.0f, 0.001f);
}

static void test_builders_match_real_frames() {
  using namespace frisquet;
  uint8_t out[64];
  struct Case { const char *name; uint8_t id; uint16_t address; uint16_t words; };
  const Case cases[] = {
      {"payload_79f2_request", 0x86, 0x79F2, 0x1C},
      {"payload_7a18_request", 0x1A, 0x7A18, 0x1C},
      {"payload_a063_request", 0x8D, 0xA063, 0x18},
      {"payload_a0fc_request", 0x1D, 0xA0FC, 0x01},
  };
  for (const Case &c : cases) {
    const Bytes &expected = F[c.name];
    const size_t n = build_read_request(out, sizeof(out), kIdConnect, 0xE3, c.id, c.address, c.words);
    CHECK(n == expected.size());
    CHECK(n == expected.size() && memcmp(out, expected.data(), n) == 0);
  }
  CHECK(build_read_request(out, 9, kIdConnect, 0xE3, 1, 0x79F2, 0x1C) == 0);

  // The acknowledgement of the relayed zone block is the exact echo the MQTT firmware sent.
  const Bytes &relay = F["payload_zone_block"];
  const Bytes &ack = F["payload_zone_ack"];
  CHECK(ack.size() == kZoneAckLength);
  CHECK(is_zone_relay(relay.data(), relay.size(), kIdConnect));
  const size_t n = build_zone_ack(relay.data(), relay.size(), kIdConnect, out, sizeof(out));
  CHECK(n == ack.size());
  CHECK(n == ack.size() && memcmp(out, ack.data(), n) == 0);

  // Not acknowledged: wrong addressee, an acknowledgement itself, a broadcast, a short buffer.
  CHECK(build_zone_ack(relay.data(), relay.size(), 0x55, out, sizeof(out)) == 0);
  CHECK(build_zone_ack(ack.data(), ack.size(), kIdConnect, out, sizeof(out)) == 0);
  const Bytes &broadcast = F["payload_sat_broadcast"];
  CHECK(build_zone_ack(broadcast.data(), broadcast.size(), kIdConnect, out, sizeof(out)) == 0);
  CHECK(build_zone_ack(relay.data(), relay.size(), kIdConnect, out, 54) == 0);
  Bytes not_zone = relay;
  not_zone[11] = 0x55;
  CHECK(!is_zone_relay(not_zone.data(), not_zone.size(), kIdConnect));
}

static void test_tx_gate() {
  using namespace frisquet;
  const Bytes &req = F["payload_79f2_request"];
  const Bytes &relay = F["payload_zone_block"];
  const Bytes &ack = F["payload_zone_ack"];
  TxPolicy p;
  p.association = 0xE3;
  TxClass cls = TxClass::kUnknown;

  // Default policy: nothing leaves.
  CHECK(evaluate_tx(p, req.data(), req.size(), nullptr, 0, &cls) == TxVerdict::kBlockedLevel);
  CHECK(cls == TxClass::kReadRequest);

  p.level = TxLevel::kRead;
  CHECK(evaluate_tx(p, req.data(), req.size()) == TxVerdict::kDryRun);
  p.dry_run = false;
  CHECK(evaluate_tx(p, req.data(), req.size()) == TxVerdict::kSend);
  // A read level does not allow the acknowledgement.
  CHECK(evaluate_tx(p, ack.data(), ack.size(), relay.data(), relay.size(), &cls) == TxVerdict::kBlockedLevel);
  CHECK(cls == TxClass::kZoneAck);
  p.level = TxLevel::kAck;
  CHECK(evaluate_tx(p, ack.data(), ack.size(), relay.data(), relay.size()) == TxVerdict::kSend);
  CHECK(evaluate_tx(p, req.data(), req.size()) == TxVerdict::kSend);

  // The acknowledgement needs the relay it answers, and must be its exact echo.
  CHECK(evaluate_tx(p, ack.data(), ack.size()) == TxVerdict::kBlockedAddress);
  Bytes tampered = ack;
  tampered[20] ^= 0x01;  // a different setpoint than the relayed block
  CHECK(evaluate_tx(p, tampered.data(), tampered.size(), relay.data(), relay.size()) == TxVerdict::kBlockedAddress);

  // Kill switch overrides everything.
  p.kill_switch = true;
  CHECK(evaluate_tx(p, req.data(), req.size()) == TxVerdict::kBlockedKillSwitch);
  p.kill_switch = false;

  // Identity: not associated, wrong association, wrong sender.
  TxPolicy unassociated = p;
  unassociated.association = 0xFF;
  CHECK(evaluate_tx(unassociated, req.data(), req.size()) == TxVerdict::kBlockedIdentity);
  TxPolicy other = p;
  other.association = 0x11;
  CHECK(evaluate_tx(other, req.data(), req.size()) == TxVerdict::kBlockedIdentity);
  TxPolicy not_connect = p;
  not_connect.self_id = 0x08;
  CHECK(evaluate_tx(not_connect, req.data(), req.size()) == TxVerdict::kBlockedIdentity);

  // Reads outside the whitelist or too large are refused, whatever the level.
  uint8_t frame[10];
  build_read_request(frame, sizeof(frame), kIdConnect, 0xE3, 1, 0xA154, 0x18);  // the zone block
  CHECK(evaluate_tx(p, frame, sizeof(frame)) == TxVerdict::kBlockedAddress);
  build_read_request(frame, sizeof(frame), kIdConnect, 0xE3, 1, 0x79F2, 0x41);
  CHECK(evaluate_tx(p, frame, sizeof(frame)) == TxVerdict::kBlockedAddress);
  build_read_request(frame, sizeof(frame), kIdConnect, 0xE3, 1, 0x79F2, 0);
  CHECK(evaluate_tx(p, frame, sizeof(frame)) == TxVerdict::kBlockedAddress);

  // Anything else, even at the highest level: association frames, zone writes, other types.
  p.level = TxLevel::kWrite;
  const Bytes assoc = {0x7E, 0x80, 0xE3, 0x01, 0x01, 0x41, 0x00};
  CHECK(evaluate_tx(p, assoc.data(), assoc.size()) == TxVerdict::kBlockedUnknownFrame);
  Bytes zone_write = relay;
  zone_write[0] = kIdBoiler;
  zone_write[1] = kIdConnect;
  zone_write[4] = 0x08;  // a write, not an acknowledgement
  CHECK(evaluate_tx(p, zone_write.data(), zone_write.size()) == TxVerdict::kBlockedUnknownFrame);
  CHECK(evaluate_tx(p, req.data(), 9) == TxVerdict::kMalformed);
  CHECK(evaluate_tx(p, nullptr, 0) == TxVerdict::kMalformed);
}

static void test_read_transaction() {
  using namespace frisquet;
  uint8_t frame[64];
  size_t length = 0;
  const Bytes &answer = F["payload_79f2_idle"];  // id 0x86, association 0xE3

  // Answer after the third transmission.
  ReadTransaction t;
  t.start(kIdConnect, 0xE3, 0x86, 0x79F2, 0x1C);
  CHECK(t.active());
  CHECK(t.poll(1000, frame, sizeof(frame), length) == ReadTransaction::Action::kSend);
  CHECK(length == 10 && frame[3] == 0x86 && be16(frame + 6) == 0x79F2);
  CHECK(t.poll(1100, frame, sizeof(frame), length) == ReadTransaction::Action::kNone);
  CHECK(t.poll(3200, frame, sizeof(frame), length) == ReadTransaction::Action::kSend);
  CHECK(t.poll(5400, frame, sizeof(frame), length) == ReadTransaction::Action::kSend);
  // Wrong id and wrong reception are ignored.
  Bytes wrong_id = answer;
  wrong_id[3] = 0x87;
  CHECK(!t.on_frame(wrong_id.data(), wrong_id.size()));
  Bytes not_ack = answer;
  not_ack[4] = 0x01;
  CHECK(!t.on_frame(not_ack.data(), not_ack.size()));
  CHECK(t.on_frame(answer.data(), answer.size()));
  CHECK(t.done() && !t.active());
  CHECK(t.answer_length() == answer.size() && memcmp(t.answer(), answer.data(), answer.size()) == 0);
  CHECK(t.poll(9000, frame, sizeof(frame), length) == ReadTransaction::Action::kNone);

  // No answer: five sends under id 0x0A, five under 0x0B, failure 22 s after the start.
  // (Measured on the air: 13:48:00 to 13:48:22 with ids 0A then 0B.)
  ReadTransaction f;
  f.start(kIdConnect, 0xE3, 0x0A, 0x79F2, 0x1C);
  const uint32_t t0 = 50000;
  int sends_a = 0, sends_b = 0;
  bool failed = false;
  uint32_t failed_at = 0;
  for (uint32_t now = t0; now <= t0 + 30000 && !failed; now += 100) {
    const ReadTransaction::Action action = f.poll(now, frame, sizeof(frame), length);
    if (action == ReadTransaction::Action::kSend) {
      if (frame[3] == 0x0A) ++sends_a;
      else if (frame[3] == 0x0B) ++sends_b;
    } else if (action == ReadTransaction::Action::kFailed) {
      failed = true;
      failed_at = now - t0;
    }
  }
  CHECK(sends_a == 5 && sends_b == 5);
  CHECK(failed);
  CHECK(failed_at == 22000);
  CHECK(f.failed());
  CHECK(!f.on_frame(answer.data(), answer.size()));

  // An answer to the second attempt (id 0x0B) is accepted.
  ReadTransaction g;
  g.start(kIdConnect, 0xE3, 0x0A, 0x79F2, 0x1C);
  for (uint32_t now = 0; now <= 11000; now += 100) g.poll(now, frame, sizeof(frame), length);
  CHECK(g.message_id() == 0x0B);
  Bytes late = answer;
  late[3] = 0x0B;
  CHECK(g.on_frame(late.data(), late.size()));
}

static void test_boiler_clock() {
  using namespace frisquet;
  CHECK(days_from_civil(1970, 1, 1) == 0);
  CHECK(days_from_civil(2000, 1, 1) == 10957);

  BoilerClock clock;
  CHECK(!clock.known());
  CHECK(clock.seconds(7000) == 7);  // unknown: uptime seconds
  // The 17:57:03 broadcast of 2026-10-25.
  CHECK(clock.set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03));
  CHECK(clock.known());
  CHECK(clock.seconds(0) == 1792951023u);
  CHECK(clock.seconds(5000) == 1792951028u);
  CHECK(clock.seconds(0) % 600 == 423);  // 7:03 into the 10-minute cycle
  // Day rollover and leap days.
  CHECK(clock.set_from_bcd(100, 0x26, 0x10, 0x26, 0x09, 0x20, 0x12));
  CHECK(clock.seconds(100) == 1793006412u);
  CHECK(clock.set_from_bcd(0, 0x26, 0x02, 0x28, 0x23, 0x59, 0x59));
  CHECK(clock.seconds(0) == 1772323199u);
  CHECK(clock.seconds(1000) == 1772323200u);
  CHECK(clock.set_from_bcd(0, 0x28, 0x02, 0x29, 0x12, 0x00, 0x00));
  CHECK(clock.seconds(0) == 1835438400u);
  // Implausible or non-BCD dates are refused and leave the clock as it was.
  CHECK(!clock.set_from_bcd(0, 0x2A, 0x10, 0x25, 0x17, 0x57, 0x03));
  CHECK(!clock.set_from_bcd(0, 0x26, 0x13, 0x25, 0x17, 0x57, 0x03));
  CHECK(!clock.set_from_bcd(0, 0x26, 0x10, 0x00, 0x17, 0x57, 0x03));
  CHECK(!clock.set_from_bcd(0, 0x26, 0x10, 0x25, 0x24, 0x57, 0x03));
  CHECK(!clock.set_from_bcd(0, 0x19, 0x10, 0x25, 0x17, 0x57, 0x03));
  CHECK(clock.seconds(0) == 1835438400u);
}

static frisquet::TxPolicy engine_policy(frisquet::TxLevel level, bool dry_run) {
  frisquet::TxPolicy p;
  p.level = level;
  p.dry_run = dry_run;
  p.association = 0xE3;
  return p;
}

static void test_engine_acknowledges_zone_relays() {
  using namespace frisquet;
  const Bytes &relay = F["payload_zone_block"];
  const Bytes &ack = F["payload_zone_ack"];
  ReadResult result;
  TxRequest tx;

  // Dry run: the acknowledgement is built exactly as the MQTT firmware sent it, not transmitted.
  ConnectEngine dry;
  dry.set_policy(engine_policy(TxLevel::kAck, true));
  CHECK(!dry.on_frame(1000, relay.data(), relay.size(), result));
  CHECK(dry.poll(1010, tx));
  CHECK(tx.verdict == TxVerdict::kDryRun && tx.kind == TxClass::kZoneAck);
  CHECK(tx.length == ack.size() && memcmp(tx.frame, ack.data(), ack.size()) == 0);
  CHECK(!dry.poll(1020, tx));  // once per relay
  CHECK(dry.stats().dry_runs == 1);

  // Live: sent, and each repetition of the relay is acknowledged again.
  ConnectEngine live;
  live.set_policy(engine_policy(TxLevel::kAck, false));
  live.on_frame(1000, relay.data(), relay.size(), result);
  CHECK(live.poll(1010, tx) && tx.verdict == TxVerdict::kSend);
  live.on_frame(5000, relay.data(), relay.size(), result);
  CHECK(live.poll(5010, tx) && tx.verdict == TxVerdict::kSend);
  CHECK(live.stats().acks_sent == 2);

  // Read-only level, kill switch, and not associated: never sent.
  ConnectEngine read_only;
  read_only.set_policy(engine_policy(TxLevel::kRead, false));
  read_only.on_frame(0, relay.data(), relay.size(), result);
  CHECK(read_only.poll(1, tx) && tx.verdict == TxVerdict::kBlockedLevel);
  CHECK(read_only.stats().acks_blocked == 1 && read_only.stats().acks_sent == 0);

  ConnectEngine killed;
  killed.set_policy(engine_policy(TxLevel::kAck, false));
  killed.set_kill_switch(true);
  killed.on_frame(0, relay.data(), relay.size(), result);
  CHECK(killed.poll(1, tx) && tx.verdict == TxVerdict::kBlockedKillSwitch);

  ConnectEngine unassociated;
  TxPolicy p = engine_policy(TxLevel::kAck, false);
  p.association = 0xFF;
  unassociated.set_policy(p);
  unassociated.on_frame(0, relay.data(), relay.size(), result);
  CHECK(unassociated.poll(1, tx) && tx.verdict == TxVerdict::kBlockedIdentity);

  // Frames that are not relays for us produce nothing.
  ConnectEngine other;
  other.set_policy(engine_policy(TxLevel::kAck, false));
  const Bytes &broadcast = F["payload_sat_broadcast"];
  other.on_frame(0, broadcast.data(), broadcast.size(), result);
  CHECK(!other.poll(1, tx));
}

static void test_engine_reads() {
  using namespace frisquet;
  const Bytes &info = F["payload_79f2_idle"];       // id 0x86
  const Bytes &consumption = F["payload_7a18"];     // id 0x1A
  ReadResult result;
  TxRequest tx;

  // Start-up reads, live: one at a time, in order, ids incrementing.
  ConnectEngine e;
  e.set_policy(engine_policy(TxLevel::kRead, false));
  e.set_next_message_id(0x86);
  e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03);  // 7:03 into the cycle: not quiet
  e.request_initial_reads();
  CHECK(e.poll(0, tx) && tx.verdict == TxVerdict::kSend && tx.kind == TxClass::kReadRequest);
  CHECK(tx.length == 10 && tx.frame[3] == 0x86 && be16(tx.frame + 6) == 0x79F2);
  CHECK(e.reading());
  CHECK(!e.poll(100, tx));  // waiting between retransmissions
  CHECK(e.poll(2300, tx) && tx.frame[3] == 0x86);  // same id, second send
  CHECK(e.on_frame(2400, info.data(), info.size(), result));
  CHECK(result.kind == ReadKind::kInfo && result.length == info.size());
  CHECK(!e.reading());
  CHECK(e.stats().reads_ok == 1);
  // Next queued read: consumption, with the next id.
  CHECK(e.poll(2500, tx) && be16(tx.frame + 6) == 0x7A18 && tx.frame[3] == 0x87);
  Bytes answer = consumption;
  answer[3] = 0x87;
  CHECK(e.on_frame(2600, answer.data(), answer.size(), result) && result.kind == ReadKind::kConsumption);
  CHECK(e.poll(2700, tx) && be16(tx.frame + 6) == 0xA063 && tx.frame[3] == 0x88);

  // Dry run: every queued read is shown once, nothing is awaited.
  ConnectEngine dry;
  dry.set_policy(engine_policy(TxLevel::kRead, true));
  dry.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03);
  dry.request_initial_reads();
  int shown = 0;
  for (int i = 0; i < 10; ++i)
    if (dry.poll(i * 10, tx)) {
      CHECK(tx.verdict == TxVerdict::kDryRun);
      ++shown;
    }
  CHECK(shown == 4 && dry.stats().dry_runs == 4 && !dry.reading());

  // Level none: reads are blocked and abandoned, not retried in a loop.
  ConnectEngine none;
  none.set_policy(engine_policy(TxLevel::kNone, false));
  none.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03);
  none.request_initial_reads();
  int blocked = 0;
  for (int i = 0; i < 10; ++i)
    if (none.poll(i * 10, tx)) {
      CHECK(tx.verdict == TxVerdict::kBlockedLevel);
      ++blocked;
    }
  CHECK(blocked == 4 && none.stats().blocked == 4);
}

static void test_engine_schedule_and_failure() {
  using namespace frisquet;
  ReadResult result;
  TxRequest tx;

  // Reads wait for the end of the quiet window.
  ConnectEngine q;
  q.set_policy(engine_policy(TxLevel::kRead, false));
  q.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x50, 0x30);  // 0:30 into the cycle
  q.request_initial_reads();
  CHECK(!q.poll(0, tx));
  CHECK(!q.poll(119000, tx));  // 2:29
  CHECK(q.poll(120000, tx) && be16(tx.frame + 6) == 0x79F2);  // 2:30: window over

  // The :03 slot (180 s into the cycle) queues a temperature read, once.
  ConnectEngine s;
  s.set_policy(engine_policy(TxLevel::kRead, true));
  s.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x52, 0x55);  // 2:55 into the cycle
  CHECK(!s.poll(0, tx));
  CHECK(!s.poll(4000, tx));
  CHECK(s.poll(5000, tx) && be16(tx.frame + 6) == 0x79F2);  // 3:00
  CHECK(!s.poll(6000, tx) && !s.poll(8000, tx));
  // :06 slot, then :08.
  CHECK(s.poll(185000, tx) && be16(tx.frame + 6) == 0x79F2);
  CHECK(s.poll(305000, tx) && be16(tx.frame + 6) == 0x79F2);
  CHECK(s.stats().reads_started == 3);

  // The hourly slots: :05 queues consumption then annual consumption, :07 the hot-water mode.
  ConnectEngine h;
  h.set_policy(engine_policy(TxLevel::kRead, true));
  h.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x18, 0x04, 0x55);  // 18:04:55
  CHECK(!h.poll(0, tx));
  CHECK(h.poll(5000, tx) && be16(tx.frame + 6) == 0x7A18);   // 18:05:00
  CHECK(h.poll(5100, tx) && be16(tx.frame + 6) == 0xA063);
  CHECK(!h.poll(5200, tx));
  CHECK(h.poll(125000, tx) && be16(tx.frame + 6) == 0xA0FC);  // 18:07:00

  // No answer: two attempts, 10 sends, the failure is counted and the id moves on by two.
  ConnectEngine f;
  f.set_policy(engine_policy(TxLevel::kRead, false));
  f.set_next_message_id(0x0A);
  f.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03);
  f.request_initial_reads();
  int sends = 0;
  uint32_t last_send_ms = 0;
  for (uint32_t now = 0; now <= 22000; now += 100) {
    if (f.poll(now, tx) && be16(tx.frame + 6) == 0x79F2) {
      ++sends;
      last_send_ms = now;
    }
  }
  CHECK(sends == 10 && last_send_ms == 19800);
  CHECK(f.stats().reads_failed == 1);
  CHECK(f.next_message_id() == 0x0C);

  // An acknowledgement due while a read is in progress goes first, and the read carries on.
  ConnectEngine a;
  a.set_policy(engine_policy(TxLevel::kAck, false));
  a.set_next_message_id(0x20);
  a.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x03);
  a.request_initial_reads();
  CHECK(a.poll(0, tx) && tx.kind == TxClass::kReadRequest);
  const Bytes &relay = F["payload_zone_block"];
  a.on_frame(500, relay.data(), relay.size(), result);
  CHECK(a.poll(600, tx) && tx.kind == TxClass::kZoneAck && tx.verdict == TxVerdict::kSend);
  CHECK(a.reading());
  CHECK(a.poll(2200, tx) && tx.kind == TxClass::kReadRequest && tx.frame[3] == 0x20);
}

static void test_zone_mode() {
  // 0x20 is Auto, reduced (verified: the satellite stayed in Auto while the programme went reduced).
  const Bytes &reduced = F["payload_sat_broadcast_reduced"];
  frisquet::SatelliteBroadcast b;
  CHECK(frisquet::decode_satellite_broadcast(reduced.data(), reduced.size(), b));
  CHECK(b.mode_raw[0] == 0x20);
  CHECK(frisquet::decode_zone_mode(b.mode_raw[0]).mode == frisquet::ZoneMode::kReducedAuto);
  CHECK_NEAR(b.setpoint[0], 17.0f, 0.001f);
  CHECK(!frisquet::decode_boiler_state(b.state_raw).running);
  CHECK(frisquet::decode_zone_mode(0x25).mode == frisquet::ZoneMode::kComfortAuto);
  CHECK(frisquet::decode_zone_mode(0x00).mode == frisquet::ZoneMode::kReducedPermanent);
  CHECK(!frisquet::decode_zone_mode(0x00).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x05).mode == frisquet::ZoneMode::kComfortAuto);

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
  const Bytes &e = F["payload_a0fc"];
  CHECK(frisquet::decode_hot_water_mode(e.data(), e.size(), m));
  CHECK(m.mode == frisquet::HotWaterMode::kEcoScheduled);
  CHECK(m.frame_bits == 0x80);

  frisquet::AnnualConsumption annual;
  const Bytes &a = F["payload_a063"];
  CHECK(a.size() == 55);
  CHECK(frisquet::decode_annual_consumption(a.data(), a.size(), annual));
  CHECK(annual.heating == 0 && annual.hot_water == 0);
  // Heating then hot water per month: set January heating=100, December hot water=7.
  Bytes synthetic = a;
  synthetic[7 + 0 * 4 + 1] = 100;
  synthetic[7 + 11 * 4 + 3] = 7;
  synthetic[7 + 5 * 4] = 0x01;  // June heating = 256
  CHECK(frisquet::decode_annual_consumption(synthetic.data(), synthetic.size(), annual));
  CHECK(annual.heating == 356);
  CHECK(annual.hot_water == 7);
  Bytes wrong_length = a;
  wrong_length[6] = 47;
  CHECK(!frisquet::decode_annual_consumption(wrong_length.data(), wrong_length.size(), annual));
}

static void test_slot_schedule() {
  using namespace frisquet;
  // Quiet window: first 150 s and last 30 s of every 10-minute cycle.
  CHECK(in_quiet_window(0) && in_quiet_window(149) && !in_quiet_window(150));
  CHECK(!in_quiet_window(569) && in_quiet_window(570) && in_quiet_window(599));
  CHECK(in_quiet_window(6000 + 10));

  SlotSchedule slot;
  const uint32_t base = 1000000 * 600;  // aligned to a cycle
  // :03 slot opens at +180 s and stays open 10 s; one attempt only.
  CHECK(!slot.due(base + 179, 1000, 600, 180));
  CHECK(slot.due(base + 180, 2000, 600, 180));
  CHECK(!slot.due(base + 181, 3000, 600, 180));
  CHECK(!slot.due(base + 189, 11000, 600, 180));
  CHECK(!slot.due(base + 190, 12000, 600, 180));  // window closed
  // Next cycle opens again.
  CHECK(slot.due(base + 600 + 183, 2000 + 600000, 600, 180));

  // A clock correction that replays the same slot must not trigger a second attempt.
  SlotSchedule corrected;
  CHECK(corrected.due(base + 360, 5000, 600, 360));
  CHECK(!corrected.due(base + 362 - 30, 5500, 600, 360));
  CHECK(!corrected.due(base + 365, 6000, 600, 360));

  // Offsets inside the quiet window and invalid parameters never fire.
  SlotSchedule other;
  CHECK(!other.due(base + 60, 1, 600, 60));
  CHECK(!other.due(base + 200, 1, 0, 0));
  CHECK(!other.due(base + 200, 1, 600, 600));

  // Hourly slots: :05 of the hour.
  SlotSchedule hourly;
  const uint32_t hour = 1000 * 3600;
  CHECK(!hourly.due(hour + 299, 10, kConsumptionPeriod, kConsumptionOffset));
  CHECK(hourly.due(hour + 300, 20, kConsumptionPeriod, kConsumptionOffset));
  CHECK(!hourly.due(hour + 3600 + 300, 20 + 3000, kConsumptionPeriod, kConsumptionOffset));
  CHECK(hourly.due(hour + 3600 + 300, 20 + 3600000, kConsumptionPeriod, kConsumptionOffset));

  // millis() wrap: the elapsed time is computed with unsigned subtraction.
  SlotSchedule wrapped;
  CHECK(wrapped.due(base + 480, 0xFFFFFF00u, 600, 480));
  CHECK(!wrapped.due(base + 481, 0x00000100u, 600, 480));
  CHECK(wrapped.due(base + 600 + 480, 0xFFFFFF00u + 600000u, 600, 480));

  // The default Connect slots are outside the quiet window.
  for (uint32_t offset : kTemperatureOffsets) CHECK(!in_quiet_window(offset));
  CHECK(!in_quiet_window(kConsumptionOffset) && !in_quiet_window(kHotWaterModeOffset));
}

static void test_read_tracker() {
  using frisquet::ReadKind;
  frisquet::ReadTracker tracker;
  ReadKind kind = ReadKind::kUnknown;

  // Answers are matched to the request seen earlier, by requester and message id.
  const Bytes &req_info = F["payload_79f2_request"];
  const Bytes &ans_info = F["payload_79f2_idle"];
  CHECK(!tracker.on_frame(req_info.data(), req_info.size(), 1000, kind));
  CHECK(tracker.on_frame(ans_info.data(), ans_info.size(), 1500, kind));
  CHECK(kind == ReadKind::kInfo);
  // An answer is consumed once.
  CHECK(!tracker.on_frame(ans_info.data(), ans_info.size(), 1600, kind));

  const Bytes &req_cons = F["payload_7a18_request"];
  const Bytes &ans_cons = F["payload_7a18"];
  const Bytes &req_hw = F["payload_a0fc_request"];
  const Bytes &ans_hw = F["payload_a0fc"];
  const Bytes &req_year = F["payload_a063_request"];
  const Bytes &ans_year = F["payload_a063"];
  tracker.on_frame(req_cons.data(), req_cons.size(), 2000, kind);
  tracker.on_frame(req_hw.data(), req_hw.size(), 2100, kind);
  tracker.on_frame(req_year.data(), req_year.size(), 2200, kind);
  CHECK(tracker.on_frame(ans_year.data(), ans_year.size(), 2300, kind));
  CHECK(kind == ReadKind::kAnnualConsumption);
  CHECK(tracker.on_frame(ans_hw.data(), ans_hw.size(), 2400, kind));
  CHECK(kind == ReadKind::kHotWaterMode);
  CHECK(tracker.on_frame(ans_cons.data(), ans_cons.size(), 2500, kind));
  CHECK(kind == ReadKind::kConsumption);

  // Answer without a request, or too late, is ignored.
  CHECK(!tracker.on_frame(ans_info.data(), ans_info.size(), 3000, kind));
  tracker.on_frame(req_info.data(), req_info.size(), 10000, kind);
  CHECK(!tracker.on_frame(ans_info.data(), ans_info.size(), 10000 + frisquet::ReadTracker::kTimeoutMs + 1, kind));

  // Retransmitted requests do not exhaust the table.
  for (uint32_t i = 0; i < 50; ++i) tracker.on_frame(req_info.data(), req_info.size(), 20000 + i, kind);
  CHECK(tracker.on_frame(ans_info.data(), ans_info.size(), 20100, kind));
  CHECK(kind == ReadKind::kInfo);

  // Boiler family offset and unknown addresses.
  CHECK(frisquet::classify_read_address(0x79F2 + 0xC8) == ReadKind::kInfo);
  CHECK(frisquet::classify_read_address(0x1234) == ReadKind::kUnknown);

  // Frames that are not reads are ignored.
  const Bytes &broadcast = F["payload_sat_broadcast"];
  CHECK(!tracker.on_frame(broadcast.data(), broadcast.size(), 30000, kind));
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
  test_builders_match_real_frames();
  test_tx_gate();
  test_read_transaction();
  test_boiler_clock();
  test_engine_acknowledges_zone_relays();
  test_engine_reads();
  test_engine_schedule_and_failure();
  test_zone_block_and_fault_broadcast();
  test_activity();
  test_other_reads();
  test_read_tracker();
  test_slot_schedule();
  test_crc_repair();
  test_crc_valid_and_invalid();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
