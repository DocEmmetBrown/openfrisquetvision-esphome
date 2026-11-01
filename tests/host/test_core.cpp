// Host tests for the pure C++ core, replaying frames captured on a real boiler.
// Build and run: tests/host/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../components/frisquet/proto_crc.h"
#include "../../components/frisquet/proto_decode.h"
#include "../../components/frisquet/proto_burner.h"
#include "../../components/frisquet/proto_engine.h"
#include "../../components/frisquet/proto_pairing.h"
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
  CHECK(frisquet::burner_firing(info.flue));
  frisquet::BoilerInfo idle;
  CHECK(frisquet::decode_boiler_info(F["payload_79f2_idle"].data(), F["payload_79f2_idle"].size(), idle));
  CHECK(!frisquet::burner_firing(idle.flue));
  CHECK(!frisquet::burner_firing(18.0f));  // still warm just after a stop (27/10 11:09)
  CHECK(!frisquet::burner_firing(NAN));
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
  CHECK(plain_mode.mode == frisquet::ZoneMode::kComfortPermanent);  // 0x05 = Confort (verified 29/10)
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

  // The same holds for a second real pair, captured live from the MQTT firmware on 2026-10-26.
  const Bytes &relay_live = F["payload_zone_relay_live"];
  const Bytes &ack_live = F["payload_zone_ack_live"];
  const size_t n_live = build_zone_ack(relay_live.data(), relay_live.size(), kIdConnect, out, sizeof(out));
  CHECK(n_live == ack_live.size());
  CHECK(n_live == ack_live.size() && memcmp(out, ack_live.data(), n_live) == 0);
  TxPolicy live_policy;
  live_policy.level = TxLevel::kAck;
  live_policy.dry_run = false;
  live_policy.association = 0xE3;
  CHECK(evaluate_tx(live_policy, ack_live.data(), ack_live.size(), relay_live.data(), relay_live.size()) ==
        TxVerdict::kSend);

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

  // Anything else, even at the highest level: association frames other than an exact confirmation
  // (see test_pairing), zone writes, other types.
  p.level = TxLevel::kWrite;
  const Bytes assoc = {0x7E, 0x80, 0xE3, 0x01, 0x01, 0x41, 0x00};
  CHECK(evaluate_tx(p, assoc.data(), assoc.size()) == TxVerdict::kMalformed);
  Bytes zone_write = relay;
  zone_write[0] = kIdBoiler;
  zone_write[1] = kIdConnect;
  zone_write[4] = 0x08;  // a write, not an acknowledgement
  CHECK(evaluate_tx(p, zone_write.data(), zone_write.size()) == TxVerdict::kBlockedAddress);  // relay header, not a write
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
  e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);  // 7:15 into the cycle: not quiet, no slot
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
  dry.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);
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
  none.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);
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

  // The :03 slot (180 s into the cycle) queues a temperature read and a state read, once.
  ConnectEngine s;
  s.set_policy(engine_policy(TxLevel::kRead, true));
  s.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x52, 0x55);  // 2:55 into the cycle
  CHECK(!s.poll(0, tx));
  CHECK(!s.poll(4000, tx));
  CHECK(s.poll(5000, tx) && be16(tx.frame + 6) == 0x79F2);  // 3:00
  CHECK(s.poll(5100, tx) && be16(tx.frame + 6) == 0xA029 && be16(tx.frame + 8) == 0x15);
  CHECK(!s.poll(6000, tx) && !s.poll(8000, tx));
  // A state read every minute, the temperatures again at :08 only (the boiler refreshes them then).
  CHECK(s.poll(65000, tx) && be16(tx.frame + 6) == 0xA029);   // :04
  CHECK(!s.poll(65100, tx));
  CHECK(s.poll(305000, tx) && be16(tx.frame + 6) == 0x79F2);  // :08
  CHECK(s.poll(305100, tx) && be16(tx.frame + 6) == 0xA029);
  CHECK(s.stats().reads_started == 5);

  // The hourly slots: :05 queues consumption then annual consumption, :07 the hot-water mode.
  ConnectEngine h;
  h.set_policy(engine_policy(TxLevel::kRead, true));
  h.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x18, 0x04, 0x55);  // 18:04:55
  CHECK(!h.poll(0, tx));
  CHECK(h.poll(5000, tx) && be16(tx.frame + 6) == 0xA029);   // 18:05:00, the minute's state read first
  CHECK(h.poll(5100, tx) && be16(tx.frame + 6) == 0x7A18);
  CHECK(h.poll(5200, tx) && be16(tx.frame + 6) == 0xA063);
  CHECK(!h.poll(5300, tx));
  CHECK(h.poll(65000, tx) && be16(tx.frame + 6) == 0xA029);   // 18:06:00
  CHECK(h.poll(125000, tx) && be16(tx.frame + 6) == 0xA029);  // 18:07:00
  CHECK(h.poll(125100, tx) && be16(tx.frame + 6) == 0xA0FC);

  // The clock switches from uptime to the boiler's date after a first attempt: the slots of the new
  // time base must still fire (regression from the dry run: :03 and :06 were skipped after the sync).
  ConnectEngine u;
  u.set_policy(engine_policy(TxLevel::kRead, true));
  CHECK(!u.poll(100000, tx));
  CHECK(u.poll(180000, tx) && be16(tx.frame + 6) == 0x79F2);  // uptime-based :03 slot, 180 s after boot
  CHECK(u.poll(180100, tx) && be16(tx.frame + 6) == 0xA029);
  // The first broadcast arrives at 200 s: boiler time 12:12:30, so the :03 slot is 30 s away.
  u.clock().set_from_bcd(200000, 0x26, 0x10, 0x26, 0x12, 0x12, 0x30);
  CHECK(!u.poll(229000, tx));
  CHECK(u.poll(230000, tx) && be16(tx.frame + 6) == 0x79F2);  // 12:13:00
  CHECK(u.poll(230100, tx) && be16(tx.frame + 6) == 0xA029);
  CHECK(u.poll(530000, tx) && be16(tx.frame + 6) == 0x79F2);  // 12:18:00 (:08)

  // No answer: two attempts, 10 sends, the failure is counted and the id moves on by two.
  ConnectEngine f;
  f.set_policy(engine_policy(TxLevel::kRead, false));
  f.set_next_message_id(0x0A);
  f.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);
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
  a.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);
  a.request_initial_reads();
  CHECK(a.poll(0, tx) && tx.kind == TxClass::kReadRequest);
  const Bytes &relay = F["payload_zone_block"];
  a.on_frame(500, relay.data(), relay.size(), result);
  CHECK(a.poll(600, tx) && tx.kind == TxClass::kZoneAck && tx.verdict == TxVerdict::kSend);
  CHECK(a.reading());
  CHECK(a.poll(2200, tx) && tx.kind == TxClass::kReadRequest && tx.frame[3] == 0x20);
}

static void test_hot_water_write_frame_and_gate() {
  using namespace frisquet;
  uint8_t out[32];
  // Derived from the MQTT firmware's envoyerModeECS(), and confirmed against a real capture (2026-10-26 14:23).
  const Bytes &real_write = F["payload_a0fc_write_eco"];
  CHECK(real_write.size() == kHotWaterModeWriteLength);
  const size_t n_real = build_hot_water_mode_write(out, sizeof(out), kIdConnect, 0xE3, 0x1C, HotWaterMode::kEco, 0x80);
  CHECK(n_real == real_write.size() && memcmp(out, real_write.data(), n_real) == 0);
  TxPolicy real_policy;
  real_policy.level = TxLevel::kWrite;
  real_policy.dry_run = false;
  real_policy.association = 0xE3;
  CHECK(evaluate_tx(real_policy, real_write.data(), real_write.size()) == TxVerdict::kSend);
  // The way back: Eco Horaires, mode byte 0x90 (frame bit 0x80 preserved).
  const Bytes &real_back = F["payload_a0fc_write_eco_scheduled"];
  const size_t n_back =
      build_hot_water_mode_write(out, sizeof(out), kIdConnect, 0xE3, 0x1D, HotWaterMode::kEcoScheduled, 0x80);
  CHECK(n_back == real_back.size() && memcmp(out, real_back.data(), n_back) == 0);
  CHECK(evaluate_tx(real_policy, real_back.data(), real_back.size()) == TxVerdict::kSend);
  // The boiler's reply repeats the written value: the read-back and the reply agree.
  const Bytes &reply_back = F["payload_a0fc_write_eco_scheduled_reply"];
  CHECK(reply_back.size() == 9 && reply_back[8] == real_back[16] && reply_back[3] == real_back[3]);
  const size_t n = build_hot_water_mode_write(out, sizeof(out), kIdConnect, 0xE3, 0x8A, HotWaterMode::kEco, 0x80);
  const uint8_t expected[17] = {0x80, 0x7E, 0xE3, 0x8A, 0x01, 0x17, 0xA0, 0xFC, 0x00,
                                0x01, 0xA0, 0xFC, 0x00, 0x01, 0x02, 0x00, 0x88};
  CHECK(n == sizeof(expected) && memcmp(out, expected, n) == 0);
  CHECK(encode_hot_water_mode(HotWaterMode::kEcoScheduled, 0x80) == 0x90);  // the value read on this boiler
  CHECK(encode_hot_water_mode(HotWaterMode::kStop, 0x01) == 0x29);
  CHECK(build_hot_water_mode_write(out, 16, kIdConnect, 0xE3, 1, HotWaterMode::kEco, 0) == 0);
  CHECK(build_hot_water_mode_write(out, sizeof(out), kIdConnect, 0xE3, 1, HotWaterMode::kUnknown, 0) == 0);

  TxPolicy p;
  p.association = 0xE3;
  p.dry_run = false;
  TxClass cls = TxClass::kUnknown;
  p.level = TxLevel::kAck;
  CHECK(evaluate_tx(p, expected, sizeof(expected), nullptr, 0, &cls) == TxVerdict::kBlockedLevel);
  CHECK(cls == TxClass::kHotWaterWrite);
  p.level = TxLevel::kRead;
  CHECK(evaluate_tx(p, expected, sizeof(expected)) == TxVerdict::kBlockedLevel);
  p.level = TxLevel::kWrite;
  CHECK(evaluate_tx(p, expected, sizeof(expected)) == TxVerdict::kSend);
  p.dry_run = true;
  CHECK(evaluate_tx(p, expected, sizeof(expected)) == TxVerdict::kDryRun);
  p.dry_run = false;

  // Only this exact write: another address, an unknown mode, another length or sender are refused.
  uint8_t bad[17];
  memcpy(bad, expected, sizeof(bad));
  bad[7] = 0xFD;  // another memory address
  CHECK(evaluate_tx(p, bad, sizeof(bad)) == TxVerdict::kBlockedAddress);
  memcpy(bad, expected, sizeof(bad));
  bad[10] = 0xA1;  // the write address differs from the read address
  CHECK(evaluate_tx(p, bad, sizeof(bad)) == TxVerdict::kBlockedAddress);
  memcpy(bad, expected, sizeof(bad));
  bad[16] = 0x38;  // 0x38 is not a hot-water mode
  CHECK(evaluate_tx(p, bad, sizeof(bad)) == TxVerdict::kBlockedAddress);
  memcpy(bad, expected, sizeof(bad));
  bad[15] = 0x01;
  CHECK(evaluate_tx(p, bad, sizeof(bad)) == TxVerdict::kBlockedAddress);
  memcpy(bad, expected, sizeof(bad));
  bad[1] = 0x08;  // not sent by the Connect
  CHECK(evaluate_tx(p, bad, sizeof(bad)) == TxVerdict::kBlockedIdentity);
  CHECK(evaluate_tx(p, expected, 16) == TxVerdict::kMalformed);
  p.kill_switch = true;
  CHECK(evaluate_tx(p, expected, sizeof(expected)) == TxVerdict::kBlockedKillSwitch);
}

static Bytes with_id(const Bytes &frame, uint8_t id) {
  Bytes copy = frame;
  copy[3] = id;
  return copy;
}

static void test_engine_hot_water_write() {
  using namespace frisquet;
  ReadResult result;
  TxRequest tx;
  uint32_t now = 0;

  // Brings an engine to the point where it knows the hot-water mode (0x90: Eco scheduled, frame bit 0x80).
  auto make_ready = [&](ConnectEngine &e, TxLevel level, bool dry_run) {
    e.set_policy(engine_policy(level, dry_run));
    e.set_next_message_id(0x86);
    e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);
    e.request_initial_reads();
    const char *answers[4] = {"payload_79f2_idle", "payload_7a18", "payload_a063", "payload_a0fc"};
    for (int i = 0; i < 4; ++i) {
      CHECK(e.poll(now, tx));
      CHECK(e.on_frame(now + 50, with_id(F[answers[i]], tx.frame[3]).data(), F[answers[i]].size(), result));
      now += 100;
    }
  };

  // Not known before the first read of 0xA0FC: the request is refused.
  ConnectEngine early;
  early.set_policy(engine_policy(TxLevel::kWrite, false));
  CHECK(!early.hot_water_mode_known());
  CHECK(!early.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(early.write_state() == WriteState::kIdle);

  // Full life: write, boiler acknowledgement, read-back, verified.
  ConnectEngine e;
  make_ready(e, TxLevel::kWrite, false);
  CHECK(e.hot_water_mode_known() && e.hot_water_mode() == HotWaterMode::kEcoScheduled);
  CHECK(!e.request_hot_water_mode(HotWaterMode::kUnknown));
  CHECK(e.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(e.write_state() == WriteState::kQueued);
  CHECK(!e.request_hot_water_mode(HotWaterMode::kMax));  // one write at a time
  CHECK(e.poll(now, tx));
  CHECK(tx.kind == TxClass::kHotWaterWrite && tx.verdict == TxVerdict::kSend && tx.length == 17);
  CHECK(tx.frame[3] == 0x8A && tx.frame[16] == 0x88);
  CHECK(e.write_state() == WriteState::kSending);
  const uint8_t write_id = tx.frame[3];
  // The boiler's INIT acknowledgement, as captured on the air: it carries the new value (00 88) back.
  const Bytes init_ack = with_id(F["payload_a0fc_write_eco_reply"], write_id);
  CHECK(!e.on_frame(now + 100, init_ack.data(), init_ack.size(), result));
  CHECK(e.write_state() == WriteState::kAcked);
  CHECK(e.stats().writes_acked == 1);
  // The read-back comes next, ahead of the scheduled reads.
  now += 200;
  CHECK(e.poll(now, tx) && tx.kind == TxClass::kReadRequest && be16(tx.frame + 6) == 0xA0FC);
  const Bytes verify = {0x7E, 0x80, 0xE3, tx.frame[3], 0x81, 0x03, 0x02, 0x00, 0x88};
  CHECK(e.on_frame(now + 50, verify.data(), verify.size(), result));
  CHECK(e.write_state() == WriteState::kVerified);
  CHECK(e.hot_water_mode() == HotWaterMode::kEco);
  CHECK(e.stats().writes_verified == 1 && e.stats().writes_failed == 0);
  CHECK(e.request_hot_water_mode(HotWaterMode::kMax));  // a new write is possible afterwards

  // The read-back shows another mode: the write is reported as failed.
  ConnectEngine mismatch;
  now = 0;
  make_ready(mismatch, TxLevel::kWrite, false);
  CHECK(mismatch.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(mismatch.poll(now, tx));
  const Bytes ack2 = {0x7E, 0x80, 0xE3, tx.frame[3], 0x81, 0x17, 0x02, 0x00, 0x88};
  mismatch.on_frame(now + 100, ack2.data(), ack2.size(), result);
  now += 200;
  CHECK(mismatch.poll(now, tx));
  const Bytes wrong = {0x7E, 0x80, 0xE3, tx.frame[3], 0x81, 0x03, 0x02, 0x00, 0x90};
  mismatch.on_frame(now + 50, wrong.data(), wrong.size(), result);
  CHECK(mismatch.write_state() == WriteState::kFailed && mismatch.stats().writes_failed == 1);

  // No acknowledgement from the boiler: ten sends in two attempts, then failed; reads still work.
  ConnectEngine silent;
  now = 0;
  make_ready(silent, TxLevel::kWrite, false);
  CHECK(silent.request_hot_water_mode(HotWaterMode::kStop));
  int sends = 0;
  const uint32_t t0 = now;
  for (uint32_t t = t0; t <= t0 + 23000; t += 100)
    if (silent.poll(t, tx) && tx.kind == TxClass::kHotWaterWrite) ++sends;
  CHECK(sends == 10);
  CHECK(silent.write_state() == WriteState::kFailed && silent.stats().writes_failed == 1);
  CHECK(!silent.reading());

  // Insufficient level or dry run: the frame is judged but never sent, and the state says so.
  ConnectEngine low;
  now = 0;
  make_ready(low, TxLevel::kAck, false);
  CHECK(low.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(low.poll(now, tx) && tx.verdict == TxVerdict::kBlockedLevel);
  CHECK(low.write_state() == WriteState::kNotSent && !low.reading());

  ConnectEngine dry;
  now = 0;
  make_ready(dry, TxLevel::kWrite, false);
  dry.set_policy(engine_policy(TxLevel::kWrite, true));  // dry-run reads would never be answered
  CHECK(dry.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(dry.poll(now, tx) && tx.verdict == TxVerdict::kDryRun && tx.kind == TxClass::kHotWaterWrite);
  CHECK(dry.write_state() == WriteState::kNotSent);

  // A write waits for the end of the satellites' quiet window.
  ConnectEngine q;
  now = 0;
  make_ready(q, TxLevel::kWrite, false);
  q.clock().set_from_bcd(now, 0x26, 0x10, 0x25, 0x18, 0x00, 0x30);  // 0:30 into the cycle
  CHECK(q.request_hot_water_mode(HotWaterMode::kEco));
  CHECK(!q.poll(now, tx));
  CHECK(q.write_state() == WriteState::kQueued);
  CHECK(q.poll(now + 120000, tx) && tx.kind == TxClass::kHotWaterWrite);
}

static void test_zone_write() {
  using namespace frisquet;
  uint8_t out[80];

  // Builder: byte for byte the write the Connect sent on 2026-10-25 17:56:04.
  const Bytes &real = F["payload_zone_write"];
  const Bytes &echo = F["payload_zone_write_reply"];
  CHECK(real.size() == kZoneWriteLength && echo.size() == kZoneAckLength);
  const size_t n = build_zone_write(out, sizeof(out), kIdConnect, 0xE3, 0x80, kIdZone1, real.data() + 15);
  CHECK(n == real.size() && memcmp(out, real.data(), n) == 0);
  CHECK(build_zone_write(out, 62, kIdConnect, 0xE3, 0x80, kIdZone1, real.data() + 15) == 0);
  CHECK(build_zone_write(out, sizeof(out), kIdConnect, 0xE3, 0x80, 0x07, real.data() + 15) == 0);
  CHECK(encode_temperature8(19.0f) == 0x8C && encode_temperature8(17.0f) == 0x78);
  CHECK(encode_temperature8(8.0f) == 0x1E && encode_temperature8(20.0f) == 0x96);

  // Gate. The reference is the block the boiler relayed last (comfort 19.0); the write moves it to 20.0.
  const Bytes &known = F["payload_zone_relay_live"];
  Bytes reference(known.begin() + 15, known.begin() + 15 + kZoneBlockSize);
  Bytes block = reference;
  block[0] = 0x96;
  TxPolicy p = engine_policy(TxLevel::kWrite, false);
  p.zone_write_mask = kZoneFieldSetpoints;
  auto frame_of = [&](const Bytes &b) {
    Bytes f(kZoneWriteLength);
    CHECK(build_zone_write(f.data(), f.size(), kIdConnect, 0xE3, 0x90, kIdZone1, b.data()) == kZoneWriteLength);
    return f;
  };
  TxClass cls = TxClass::kUnknown;
  Bytes f = frame_of(block);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, &cls, reference.data()) == TxVerdict::kSend);
  CHECK(cls == TxClass::kZoneWrite);
  CHECK(evaluate_tx(p, f.data(), f.size()) == TxVerdict::kBlockedAddress);  // no known block: nothing is sent
  TxPolicy none = p;
  none.zone_write_mask = 0;
  CHECK(evaluate_tx(none, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  TxPolicy ack_only = p;
  ack_only.level = TxLevel::kAck;
  CHECK(evaluate_tx(ack_only, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedLevel);
  TxPolicy dry = p;
  dry.dry_run = true;
  CHECK(evaluate_tx(dry, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kDryRun);
  TxPolicy killed = p;
  killed.kill_switch = true;
  CHECK(evaluate_tx(killed, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedKillSwitch);

  // Only the allowed fields may differ from the known block.
  Bytes other = block;
  other[3] = 0x07;  // mode changed, but only setpoints are allowed
  other[4] = 0x20;  // reduced: the comfort bit goes
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  TxPolicy with_mode = p;
  with_mode.zone_write_mask = kZoneFieldSetpoints | kZoneFieldMode;
  CHECK(evaluate_tx(with_mode, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kSend);
  other = block;
  other[10] ^= 0x01;  // programme changed
  f = frame_of(other);
  CHECK(evaluate_tx(with_mode, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  TxPolicy with_program = p;
  with_program.zone_write_mask = kZoneFieldProgram | kZoneFieldSetpoints;
  CHECK(evaluate_tx(with_program, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kSend);
  other = block;
  other[5] ^= 0x01;  // options byte: never allowed through a mask that excludes mode
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);

  // Plausible setpoints only: ordered, in 0.5 degree steps, within the raw range.
  other = block;
  other[1] = 0x9B;  // reduced above comfort
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  other = block;
  other[0] = 0x97;  // not a multiple of 5
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  other = block;
  other[0] = 0xFF;
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  other = block;
  other[2] = 0x7D;  // frost above reduced
  f = frame_of(other);
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  f = frame_of(block);
  f[9] = 0x15;  // header differs from the known write header
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kBlockedAddress);
  f = frame_of(block);
  f[4] = 0x0B;  // not a zone
  CHECK(evaluate_tx(p, f.data(), f.size(), nullptr, 0, nullptr, reference.data()) == TxVerdict::kMalformed);

  // Engine: the block is learned from a relay, then written in bursts until the boiler echoes it.
  ReadResult result;
  TxRequest tx;
  ConnectEngine e;
  e.set_policy(p);
  e.set_next_message_id(0x80);
  e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);  // 4:30 into the cycle: no slot, not quiet
  CHECK(!e.zone_block_known(0) && !e.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
  e.on_frame(100, known.data(), known.size(), result);
  CHECK(e.poll(110, tx) && tx.kind == TxClass::kZoneAck);
  CHECK(e.zone_block_known(0) && memcmp(e.zone_block(0), reference.data(), kZoneBlockSize) == 0);
  CHECK(!e.request_zone_setpoints(3, 0x96, 0x78, 0x1E) && !e.request_zone_setpoints(1, 0x96, 0x78, 0x1E));
  CHECK(e.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
  CHECK(e.zone_write_state() == WriteState::kQueued && e.zone_write_pending());
  CHECK(!e.request_zone_setpoints(0, 0x8C, 0x78, 0x1E));  // one write at a time

  uint32_t now = 1000;
  int sends = 0;
  uint8_t first_id = 0;
  for (; now < 12000; now += 100) {
    if (e.poll(now, tx) && tx.kind == TxClass::kZoneWrite) {
      CHECK(tx.verdict == TxVerdict::kSend && tx.length == kZoneWriteLength);
      if (sends++ == 0) first_id = tx.frame[3];
      CHECK(tx.frame[3] == first_id && tx.frame[4] == kIdZone1);
      CHECK(memcmp(tx.frame + 15, block.data(), kZoneBlockSize) == 0);
    }
  }
  CHECK(sends == 5);  // one burst: five sends under one id
  CHECK(first_id == 0x80 && e.zone_write_state() == WriteState::kSending);
  // A relay of the old block while the write is pending does not replace the known block.
  e.on_frame(12000, known.data(), known.size(), result);
  CHECK(memcmp(e.zone_block(0), reference.data(), kZoneBlockSize) == 0);
  CHECK(e.poll(12010, tx) && tx.kind == TxClass::kZoneAck);

  // The next burst starts about 30 s after the last, under the next id.
  int second = 0;
  uint8_t second_id = 0;
  for (; now < 60000; now += 100) {
    if (e.poll(now, tx) && tx.kind == TxClass::kZoneWrite) {
      if (second++ == 0) second_id = tx.frame[3];
    }
  }
  // The :05 temperature read runs between the bursts (unanswered here: two attempts, ids 0x81 and 0x82).
  CHECK(second == 4 && second_id == 0x83);
  CHECK(e.stats().zone_bursts == 2);

  // The boiler answers a send of the first burst (as seen on the air): the write is done, the block known.
  Bytes reply(kZoneAckLength);
  memcpy(reply.data(), echo.data(), 7);
  reply[3] = 0x80;
  memcpy(reply.data() + 7, block.data(), kZoneBlockSize);
  Bytes wrong_block = reply;
  wrong_block[7] ^= 0x01;
  e.on_frame(now, wrong_block.data(), wrong_block.size(), result);
  CHECK(e.zone_write_state() == WriteState::kSending);
  Bytes wrong_id = reply;
  wrong_id[3] = 0x7F;  // before this write began
  e.on_frame(now, wrong_id.data(), wrong_id.size(), result);
  CHECK(e.zone_write_state() == WriteState::kSending);
  e.on_frame(now, reply.data(), reply.size(), result);
  CHECK(e.zone_write_state() == WriteState::kVerified && e.stats().zone_writes_acked == 1);
  CHECK(memcmp(e.zone_block(0), block.data(), kZoneBlockSize) == 0);
  const uint32_t bursts = e.stats().zone_bursts;
  for (uint32_t t = now; t < now + 120000; t += 100) e.poll(t, tx);
  CHECK(e.stats().zone_bursts == bursts);  // no more sends once echoed
  CHECK(e.request_zone_setpoints(0, 0x8C, 0x78, 0x1E));  // a new write is possible

  // Changed on the satellite while we write (31/10: comfort 19.0 set by hand while we wrote 20.0): local wins.
  {
    ConnectEngine l;
    l.set_policy(p);
    l.set_next_message_id(0x80);
    l.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);
    l.on_frame(100, known.data(), known.size(), result);
    CHECK(l.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
    uint32_t lt = 1000;
    for (; lt < 3000; lt += 100) l.poll(lt, tx);
    CHECK(l.zone_write_state() == WriteState::kSending);
    Bytes local = known;
    local[15] = 0x82;  // comfort 18.0, neither the old block nor ours
    local[3] = 0x42;
    l.on_frame(lt, local.data(), local.size(), result);
    CHECK(l.zone_write_state() == WriteState::kSuperseded && !l.zone_write_pending());
    CHECK(l.zone_block(0)[0] == 0x82);
    CHECK(l.poll(lt + 10, tx) && tx.kind == TxClass::kZoneAck);  // the relay is still acknowledged
    bool more = false;
    for (uint32_t t = lt + 100; t < lt + 120000; t += 100)
      if (l.poll(t, tx) && tx.kind == TxClass::kZoneWrite) more = true;
    CHECK(!more);
    CHECK(l.request_zone_setpoints(0, 0x96, 0x78, 0x1E));  // a new write starts from the local block
  }
  // The satellite relays our block (options normalised) before the echo: the write is done.
  {
    ConnectEngine r;
    r.set_policy(p);
    r.set_next_message_id(0x80);
    r.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);
    r.on_frame(100, known.data(), known.size(), result);
    CHECK(r.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
    for (uint32_t t = 1000; t < 3000; t += 100) r.poll(t, tx);
    Bytes ours = known;
    ours[15] = 0x96;
    ours[16] = 0x78;
    ours[17] = 0x1E;
    ours[19] ^= 0x04;  // the satellite flips 0x04 by itself
    ours[3] = 0x43;
    r.on_frame(3000, ours.data(), ours.size(), result);
    CHECK(r.zone_write_state() == WriteState::kVerified && r.zone_block(0)[0] == 0x96);
  }

  // Never echoed: bursts continue until the deadline, then failed (and the block stays as it was).
  ConnectEngine g;
  g.set_policy(p);
  g.set_next_message_id(0x80);
  g.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);
  g.on_frame(100, known.data(), known.size(), result);
  CHECK(g.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
  for (uint32_t t = 1000; t < 22UL * 60UL * 1000UL; t += 100) {
    if (g.poll(t, tx) && tx.kind == TxClass::kZoneAck) continue;
  }
  CHECK(g.zone_write_state() == WriteState::kFailed && g.stats().zone_writes_failed == 1);
  CHECK(g.stats().zone_bursts >= 15);  // one temperature read a minute runs between the bursts
  CHECK(memcmp(g.zone_block(0), reference.data(), kZoneBlockSize) == 0);

  // Below the write level or in dry run: judged, never sent.
  ConnectEngine low;
  low.set_policy(ack_only);
  low.set_next_message_id(0x80);
  low.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);
  low.on_frame(100, known.data(), known.size(), result);
  CHECK(low.request_zone_setpoints(0, 0x96, 0x78, 0x1E));
  bool judged = false;
  for (uint32_t t = 1000; t < 5000 && !judged; t += 100) {
    if (low.poll(t, tx) && tx.kind == TxClass::kZoneWrite) {
      CHECK(tx.verdict == TxVerdict::kBlockedLevel);
      judged = true;
    }
  }
  CHECK(judged && low.zone_write_state() == WriteState::kNotSent && !low.reading());
}

static void test_zone_mode_and_program_write() {
  using namespace frisquet;
  const Bytes &known = F["payload_zone_relay_live"];  // mode 05, options 21, comfort 19.0
  Bytes reference(known.begin() + 15, known.begin() + 15 + kZoneBlockSize);
  ReadResult result;
  TxRequest tx;
  TxPolicy p = engine_policy(TxLevel::kWrite, false);
  p.zone_write_mask = kZoneFieldSetpoints | kZoneFieldMode | kZoneFieldProgram;

  auto make = [&](ConnectEngine &e) {
    e.set_policy(p);
    e.set_next_message_id(0x80);
    e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x54, 0x30);
    e.on_frame(100, known.data(), known.size(), result);
  };
  auto first_write = [&](ConnectEngine &e) {
    for (uint32_t t = 1000; t < 6000; t += 100)
      if (e.poll(t, tx) && tx.kind == TxClass::kZoneWrite) return true;
    return false;
  };

  // Mode: each choice sets the mode byte and only the owned option bits.
  struct Case { uint8_t mode, options; } cases[] = {{0x06, 0x01}, {0x07, 0x00}, {0x08, 0x10}};
  for (const Case &c : cases) {
    ConnectEngine e;
    make(e);
    CHECK(e.request_zone_mode(0, c.mode));
    CHECK(first_write(e) && tx.verdict == TxVerdict::kSend);
    CHECK(tx.frame[15 + 3] == c.mode && tx.frame[15 + 4] == (0x20 | c.options));  // 0x20: flag bit kept
    CHECK(memcmp(tx.frame + 15, reference.data(), 3) == 0 && memcmp(tx.frame + 15 + 5, reference.data() + 5, 43) == 0);
  }
  ConnectEngine to_auto;
  make(to_auto);
  CHECK(!to_auto.request_zone_mode(0, 0x04) && !to_auto.request_zone_mode(0, 0x09) && !to_auto.request_zone_mode(1, 0x06));
  CHECK(to_auto.request_zone_mode(0, 0x05));  // already auto: options keep the comfort bit, gets 0x25
  CHECK(first_write(to_auto) && tx.frame[15 + 3] == 0x05 && tx.frame[15 + 4] == 0x25);

  // Derogations: Auto with options 0x04 | 0x02 | comfort (MQTT firmware's Zone::setMode); the gate accepts them.
  for (int comfort = 0; comfort < 2; ++comfort) {
    ConnectEngine d;
    make(d);
    CHECK(d.request_zone_derogation(0, comfort == 1));
    CHECK(first_write(d) && tx.verdict == TxVerdict::kSend);
    CHECK(tx.frame[15 + 3] == 0x05 && tx.frame[15 + 4] == (comfort ? 0x27 : 0x26));
    CHECK(memcmp(tx.frame + 15, reference.data(), 3) == 0 && memcmp(tx.frame + 15 + 5, reference.data() + 5, 43) == 0);
    CHECK(select_mode_from_block(tx.frame + 15) == (comfort ? kSelectDerogationComfort : kSelectDerogationReduced));
  }
  ConnectEngine unknown_block;
  unknown_block.set_policy(p);
  CHECK(!unknown_block.request_zone_derogation(0, true));
  {
    // Back to Auto from a derogation clears the derogation bit, keeps the comfort bit.
    uint8_t derog[kZoneBlockSize];
    memcpy(derog, reference.data(), kZoneBlockSize);
    derog[4] = 0x27;
    CHECK(select_mode_from_block(derog) == kSelectDerogationComfort);
    derog[4] = 0x25;
    CHECK(select_mode_from_block(derog) == 0x05);
    derog[3] = 0x08;
    derog[4] = 0x10;
    CHECK(select_mode_from_block(derog) == 0x08);
  }

  // Programme: the 42 bytes are replaced, nothing else.
  uint8_t program[42];
  memcpy(program, reference.data() + 6, 42);
  program[8] = 0x00;  // a visible change in Monday's bits
  ConnectEngine prog;
  make(prog);
  CHECK(prog.request_zone_program(0, program));
  CHECK(first_write(prog) && tx.verdict == TxVerdict::kSend);
  CHECK(memcmp(tx.frame + 15 + 6, program, 42) == 0 && memcmp(tx.frame + 15, reference.data(), 6) == 0);

  // A request only ever changes its own field, even if the policy would allow more.
  Bytes crafted = reference;
  crafted[0] = 0x96;  // setpoint change smuggled into a mode request
  Bytes frame(kZoneWriteLength);
  build_zone_write(frame.data(), frame.size(), kIdConnect, 0xE3, 0x90, kIdZone1, crafted.data());
  TxPolicy only_mode = p;
  only_mode.zone_write_mask = kZoneFieldMode;
  CHECK(evaluate_tx(only_mode, frame.data(), frame.size(), nullptr, 0, nullptr, reference.data()) ==
        TxVerdict::kBlockedAddress);

  // Gate: incoherent mode/options, a foreign option bit and the unknown byte are refused.
  auto verdict = [&](uint8_t mode, uint8_t options, uint8_t unknown) {
    Bytes b = reference;
    b[3] = mode;
    b[4] = options;
    b[5] = unknown;
    build_zone_write(frame.data(), frame.size(), kIdConnect, 0xE3, 0x90, kIdZone1, b.data());
    return evaluate_tx(p, frame.data(), frame.size(), nullptr, 0, nullptr, reference.data());
  };
  CHECK(verdict(0x07, 0x20, 0x00) == TxVerdict::kSend);
  CHECK(verdict(0x07, 0x21, 0x00) == TxVerdict::kBlockedAddress);  // reduced with the comfort bit
  CHECK(verdict(0x06, 0x20, 0x00) == TxVerdict::kBlockedAddress);  // comfort without the comfort bit
  CHECK(verdict(0x08, 0x30, 0x00) == TxVerdict::kSend);
  CHECK(verdict(0x07, 0x60, 0x00) == TxVerdict::kBlockedAddress);  // boost bit is not ours
  CHECK(verdict(0x07, 0x20, 0x01) == TxVerdict::kBlockedAddress);  // unknown byte
  CHECK(verdict(0x05, 0x21, 0x00) == TxVerdict::kSend);            // unchanged block
}

static void test_program_text() {
  using namespace frisquet;
  const Bytes &known = F["payload_zone_relay_live"];
  const uint8_t *program = known.data() + 15 + 6;
  char text[160];
  format_program_ranges(program, text, sizeof(text));
  CHECK(strcmp(text, "Dim 09:30-22:00 | Lun 08:00-21:30 | Mar 08:00-21:30 | Mer 08:00-21:30 | Jeu 08:00-21:30 | "
                     "Ven 08:00-21:30 | Sam 09:30-22:00") == 0);
  char small[10];
  format_program_ranges(program, small, sizeof(small));
  CHECK(strlen(small) == 9);
  uint8_t none[42] = {};
  none[6] = 0xFF; none[7] = 0xFF; none[8] = 0xFF; none[9] = 0xFF; none[10] = 0xFF; none[11] = 0xFF;  // Monday all day
  format_program_ranges(none, text, sizeof(text));
  CHECK(strncmp(text, "Dim - | Lun 00:00-24:00 | Mar -", 31) == 0);

  char hex[85];
  format_program_hex(program, hex);
  uint8_t back[42];
  CHECK(strlen(hex) == 84 && parse_program_hex(hex, 84, back) && memcmp(back, program, 42) == 0);
  CHECK(parse_program_hex("00", 2, back) == false);
  hex[10] = 'g';
  CHECK(!parse_program_hex(hex, 84, back));
  hex[10] = 'a';
  CHECK(parse_program_hex(hex, 84, back));
}

static void test_zone_store_and_restore() {
  using namespace frisquet;
  const Bytes &relay = F["payload_zone_block"];  // 25/10 17:57:06: comfort 20.0, reduced 17.0, Auto, Sunday 09:30-22:00
  const uint8_t *block = relay.data() + 15;
  uint8_t blocks[3][kZoneBlockSize] = {};
  memcpy(blocks[0], block, kZoneBlockSize);
  memcpy(blocks[2], block, kZoneBlockSize);
  blocks[2][0] = 0x8C;

  // Record: round trip, and every kind of damage is refused.
  uint8_t record[kZoneStoreSize];
  pack_zone_store(0x05, blocks, record);
  uint8_t mask = 0;
  uint8_t back[3][kZoneBlockSize] = {};
  CHECK(unpack_zone_store(record, sizeof(record), mask, back) && mask == 0x05);
  CHECK(memcmp(back[0], blocks[0], kZoneBlockSize) == 0 && memcmp(back[2], blocks[2], kZoneBlockSize) == 0);
  uint8_t damaged[kZoneStoreSize];
  memcpy(damaged, record, sizeof(record));
  damaged[20] ^= 0x01;
  CHECK(!unpack_zone_store(damaged, sizeof(damaged), mask, back));
  memcpy(damaged, record, sizeof(record));
  damaged[4] = 2;
  CHECK(!unpack_zone_store(damaged, sizeof(damaged), mask, back));
  memcpy(damaged, record, sizeof(record));
  damaged[0] = 'X';
  CHECK(!unpack_zone_store(damaged, sizeof(damaged), mask, back));
  CHECK(!unpack_zone_store(record, sizeof(record) - 1, mask, back) && !unpack_zone_store(nullptr, 0, mask, back));
  uint8_t zeros[kZoneStoreSize] = {};
  CHECK(!unpack_zone_store(zeros, sizeof(zeros), mask, back));  // erased flash

  // Check against real broadcasts.
  SatelliteBroadcast comfort_b, reduced_b;
  CHECK(decode_satellite_broadcast(F["payload_sat_broadcast"].data(), F["payload_sat_broadcast"].size(), comfort_b));
  CHECK(decode_satellite_broadcast(F["payload_sat_broadcast_reduced"].data(),
                                   F["payload_sat_broadcast_reduced"].size(), reduced_b));
  auto check = [&](const uint8_t *b, const SatelliteBroadcast &sb) {
    return check_block_against_broadcast(b, sb.mode_raw[0], sb.setpoint[0], sb.weekday, sb.hour, sb.minute);
  };
  CHECK(check(block, comfort_b) == BlockCheck::kConsistent);  // 17:57 Sunday: comfort 20.0
  CHECK(check(block, reduced_b) == BlockCheck::kConsistent);  // 22:10 Sunday: reduced 17.0
  uint8_t changed[kZoneBlockSize];
  memcpy(changed, block, kZoneBlockSize);
  changed[0] = 0x8C;  // another comfort setpoint
  CHECK(check(changed, comfort_b) == BlockCheck::kInconsistent);
  memcpy(changed, block, kZoneBlockSize);
  changed[1] = 0x6E;  // another reduced setpoint
  CHECK(check(changed, reduced_b) == BlockCheck::kInconsistent);
  memcpy(changed, block, kZoneBlockSize);
  memset(changed + 6, 0, 6);  // Sunday has no comfort period any more
  CHECK(check(changed, comfort_b) == BlockCheck::kInconsistent);
  changed[3] = 0x07;  // reduced permanent, but the satellite is Auto/comfort
  CHECK(check(changed, comfort_b) == BlockCheck::kInconsistent);
  memcpy(changed, block, kZoneBlockSize);
  changed[3] = 0x06;
  CHECK(check(changed, reduced_b) == BlockCheck::kInconsistent);
  // Next to a half-hour boundary, a programme mismatch proves nothing.
  memcpy(changed, block, kZoneBlockSize);
  memset(changed + 6, 0, 6);
  CHECK(check_block_against_broadcast(changed, comfort_b.mode_raw[0], comfort_b.setpoint[0], comfort_b.weekday, 0x17,
                                      0x31) == BlockCheck::kInconclusive);
  CHECK(check_block_against_broadcast(block, 0xFF, 20.0f, 0, 0x17, 0x57) == BlockCheck::kInconclusive);
  CHECK(check_block_against_broadcast(block, comfort_b.mode_raw[0], NAN, 0, 0x17, 0x57) == BlockCheck::kInconclusive);

  // Engine: a restored block is shown but not writable until a relay or a consistent broadcast confirms it.
  const Bytes &known = F["payload_zone_block"];
  TxPolicy p = engine_policy(TxLevel::kWrite, false);
  p.zone_write_mask = kZoneFieldSetpoints | kZoneFieldMode | kZoneFieldProgram;
  ReadResult result;

  ConnectEngine e;
  e.set_policy(p);
  e.restore_zone_block(0, block);
  CHECK(e.zone_block_known(0) && !e.zone_block_confirmed(0) && !e.take_zone_dirty());
  CHECK(!e.request_zone_setpoints(0, 0x96, 0x78, 0x1E) && !e.request_zone_mode(0, 0x07));
  uint8_t program[42] = {};
  CHECK(!e.request_zone_program(0, program));
  e.on_zone_broadcast(0, comfort_b.mode_raw[0], comfort_b.setpoint[0], comfort_b.weekday, comfort_b.hour,
                      comfort_b.minute);
  CHECK(e.zone_block_confirmed(0) && !e.take_zone_dirty());
  CHECK(e.request_zone_setpoints(0, 0x9B, 0x78, 0x1E));

  // An inconsistent broadcast discards the copy and asks for the (now empty) store to be rewritten.
  ConnectEngine stale;
  stale.set_policy(p);
  memcpy(changed, block, kZoneBlockSize);
  changed[0] = 0x8C;
  stale.restore_zone_block(0, changed);
  stale.on_zone_broadcast(0, comfort_b.mode_raw[0], comfort_b.setpoint[0], comfort_b.weekday, comfort_b.hour,
                          comfort_b.minute);
  CHECK(!stale.zone_block_known(0) && stale.take_zone_dirty() && !stale.take_zone_dirty());
  CHECK(!stale.request_zone_setpoints(0, 0x96, 0x78, 0x1E));

  // A relay confirms a restored block (and a changed one replaces it); repeated identical relays are not re-persisted.
  ConnectEngine r;
  r.set_policy(p);
  r.restore_zone_block(0, changed);
  r.on_frame(100, known.data(), known.size(), result);
  CHECK(r.zone_block_confirmed(0) && memcmp(r.zone_block(0), block, kZoneBlockSize) == 0);
  CHECK(r.take_zone_dirty() && !r.take_zone_dirty());
  r.on_frame(5000, known.data(), known.size(), result);
  CHECK(!r.take_zone_dirty());

  // A broadcast never overrides a confirmed block, nor one with a write pending.
  r.on_zone_broadcast(0, 0x00, 5.0f, 0, 0x12, 0x10);
  CHECK(r.zone_block_known(0) && r.zone_block_confirmed(0));

  // The mask of known blocks for persistence.
  CHECK(r.zone_known_mask() == 0x01 && memcmp(r.zone_blocks()[0], block, kZoneBlockSize) == 0);
}

static void test_zone_mode() {
  // Exploratory reads: any address, at most 0x1C words, only with `explore: true`.
  {
    using namespace frisquet;
    uint8_t request[kReadRequestLength];
    build_read_request(request, sizeof(request), kIdConnect, 0xD5, 1, 0x7A00, 0x1C);
    TxPolicy p;
    p.level = TxLevel::kRead;
    p.dry_run = false;
    p.association = 0xD5;
    CHECK(evaluate_tx(p, request, sizeof(request)) == TxVerdict::kBlockedAddress);
    p.explore_reads = true;
    CHECK(evaluate_tx(p, request, sizeof(request)) == TxVerdict::kSend);
    build_read_request(request, sizeof(request), kIdConnect, 0xD5, 1, 0x7A00, 0x1D);
    CHECK(evaluate_tx(p, request, sizeof(request)) == TxVerdict::kBlockedAddress);
    p.level = TxLevel::kNone;
    build_read_request(request, sizeof(request), kIdConnect, 0xD5, 1, 0x7A00, 0x10);
    CHECK(evaluate_tx(p, request, sizeof(request)) == TxVerdict::kBlockedLevel);

    ReadResult result;
    TxRequest tx;
    ConnectEngine e;
    TxPolicy ep = engine_policy(TxLevel::kRead, false);
    ep.association = 0xD5;
    e.set_policy(ep);
    e.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x17, 0x57, 0x15);  // no slot due
    CHECK(!e.request_explore_read(0x7A00, 0x10));  // not allowed by the policy
    ep.explore_reads = true;
    e.set_policy(ep);
    CHECK(!e.request_explore_read(0x7A00, 0));
    CHECK(!e.request_explore_read(0x7A00, 0x1D));
    CHECK(e.request_explore_read(0x7A00, 0x02) && e.explore_pending());
    CHECK(!e.request_explore_read(0x7B00, 0x02));  // one at a time
    CHECK(e.poll(100, tx) && be16(tx.frame + 6) == 0x7A00 && be16(tx.frame + 8) == 0x02);
    CHECK(tx.verdict == TxVerdict::kSend);
    const Bytes answer = {0x7E, 0x80, 0xD5, tx.frame[3], 0x81, 0x03, 0x04, 0x12, 0x34, 0x56, 0x78};
    CHECK(e.on_frame(200, answer.data(), answer.size(), result));
    CHECK(result.kind == ReadKind::kExplore && result.address == 0x7A00 && result.length == answer.size());
    CHECK(!e.explore_pending() && !e.take_explore_failed());

    // Unanswered: reported once as failed.
    CHECK(e.request_explore_read(0x7B00, 0x04));
    for (uint32_t t = 300; t < 40000; t += 100) e.poll(t, tx);
    CHECK(!e.explore_pending() && e.take_explore_failed() && !e.take_explore_failed());
    CHECK(!e.explore_refused());

    // Refused by the boiler (type 0x83): ends at once, flagged as refused; a refusal under another id is ignored.
    CHECK(e.request_explore_read(0x7B10, 0x04));
    CHECK(e.poll(40100, tx) && be16(tx.frame + 6) == 0x7B10);
    const Bytes other_id = {0x7E, 0x80, 0xD5, static_cast<uint8_t>(tx.frame[3] + 1), 0x81, 0x83, 0x02};
    e.on_frame(40150, other_id.data(), other_id.size(), result);
    CHECK(e.explore_pending());
    const Bytes refusal = {0x7E, 0x80, 0xD5, tx.frame[3], 0x81, 0x83, 0x02};
    CHECK(!e.on_frame(40200, refusal.data(), refusal.size(), result));
    e.poll(40300, tx);
    CHECK(!e.explore_pending() && e.take_explore_failed() && e.explore_refused());

    // Dry run: nothing awaited, reported as failed.
    ep.dry_run = true;
    e.set_policy(ep);
    CHECK(e.request_explore_read(0x7C00, 0x04));
    CHECK(e.poll(50000, tx) && tx.verdict == TxVerdict::kDryRun);
    CHECK(!e.explore_pending() && e.take_explore_failed());
  }
  // 0xA029 read answer: the broadcast data under a read-answer header (7E 80 <assoc> <id> 81 03 2A ...).
  {
    using namespace frisquet;
    Bytes answer = F["payload_sat_broadcast_reduced"];
    const Bytes header = {0x7E, 0x80, 0xD5, 0x42, 0x81, 0x03};
    std::copy(header.begin(), header.end(), answer.begin());
    SatelliteBroadcast from_read, from_broadcast;
    CHECK(decode_status_read(answer.data(), answer.size(), from_read));
    CHECK(decode_satellite_broadcast(F["payload_sat_broadcast_reduced"].data(),
                                     F["payload_sat_broadcast_reduced"].size(), from_broadcast));
    CHECK(from_read.state_raw == from_broadcast.state_raw && from_read.mode_raw[0] == from_broadcast.mode_raw[0]);
    CHECK(from_read.minute == from_broadcast.minute && from_read.weekday == from_broadcast.weekday);
    CHECK(!decode_satellite_broadcast(answer.data(), answer.size(), from_broadcast));
    CHECK(!decode_status_read(F["payload_sat_broadcast_reduced"].data(), F["payload_sat_broadcast_reduced"].size(),
                              from_read));
    CHECK(classify_read_address(0xA029) == ReadKind::kStatus);
    uint8_t request[kReadRequestLength];
    build_read_request(request, sizeof(request), kIdConnect, 0xD5, 1, 0xA029, 0x15);
    TxPolicy p;
    p.level = TxLevel::kRead;
    p.dry_run = false;
    p.association = 0xD5;
    CHECK(evaluate_tx(p, request, sizeof(request)) == TxVerdict::kSend);
  }
  // 0x20 is Auto, reduced (verified: the satellite stayed in Auto while the programme went reduced).
  const Bytes &reduced = F["payload_sat_broadcast_reduced"];
  frisquet::SatelliteBroadcast b;
  CHECK(frisquet::decode_satellite_broadcast(reduced.data(), reduced.size(), b));
  CHECK(b.mode_raw[0] == 0x20);
  CHECK(frisquet::decode_zone_mode(b.mode_raw[0]).mode == frisquet::ZoneMode::kReducedAuto);
  CHECK_NEAR(b.setpoint[0], 17.0f, 0.001f);
  CHECK(!frisquet::decode_boiler_state(b.state_raw).running);
  CHECK(frisquet::decode_zone_mode(0x25).mode == frisquet::ZoneMode::kComfortAuto);
  CHECK(frisquet::decode_zone_mode(0x21).mode == frisquet::ZoneMode::kComfortAuto);  // seen 10:37 after the restore
  CHECK(frisquet::decode_zone_mode(0x21).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x01).mode == frisquet::ZoneMode::kComfortPermanent);
  CHECK(frisquet::decode_zone_mode(0x23).mode == frisquet::ZoneMode::kComfortOverride);
  CHECK(frisquet::decode_zone_mode(0x22).mode == frisquet::ZoneMode::kReducedOverride);
  CHECK(frisquet::decode_zone_mode(0x24).mode == frisquet::ZoneMode::kReducedAuto);
  CHECK(frisquet::decode_zone_mode(0x40).mode == frisquet::ZoneMode::kUnknown);
  CHECK(frisquet::decode_zone_mode(0x00).mode == frisquet::ZoneMode::kReducedPermanent);
  CHECK(!frisquet::decode_zone_mode(0x00).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x05).mode == frisquet::ZoneMode::kComfortPermanent);  // 29/10, Confort set
  CHECK(frisquet::decode_zone_mode(0x04).mode == frisquet::ZoneMode::kReducedPermanent);

  CHECK(frisquet::decode_zone_mode(0x10).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(frisquet::decode_zone_mode(0x30).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(frisquet::decode_zone_mode(0x30).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x77).mode == frisquet::ZoneMode::kUnknown);

  // Hors gel set on the satellite (28/10 22:40 and 22:50): 0x14, without the 0x20 flag.
  CHECK(frisquet::decode_zone_mode(0x14).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(!frisquet::decode_zone_mode(0x14).connect_compat);
  CHECK(frisquet::decode_zone_mode(0x11).mode == frisquet::ZoneMode::kFrostProtection);
  CHECK(frisquet::select_mode_from_broadcast(0x14) == 0x08);
  CHECK(frisquet::select_mode_from_broadcast(0x30) == 0x08);
  CHECK(frisquet::select_mode_from_broadcast(0x25) == 0x05);
  CHECK(frisquet::select_mode_from_broadcast(0x20) == 0x05);
  CHECK(frisquet::select_mode_from_broadcast(0x23) == frisquet::kSelectDerogationComfort);
  CHECK(frisquet::select_mode_from_broadcast(0x22) == frisquet::kSelectDerogationReduced);
  CHECK(frisquet::select_mode_from_broadcast(0x05) == 0x06);
  CHECK(frisquet::select_mode_from_broadcast(0x00) == 0x07);
  CHECK(frisquet::select_mode_from_broadcast(0x01) == 0x06);
  CHECK(frisquet::select_mode_from_broadcast(0x40) == 0);
  // The block saved in Auto on 27/10 must be discarded by that broadcast (it stayed "restauré" on 28/10).
  uint8_t block[frisquet::kZoneBlockSize] = {0x96, 0x78, 0x1E, 0x05, 0x21};
  CHECK(frisquet::check_block_against_broadcast(block, 0x14, 8.0f, 3, 0x22, 0x50) ==
        frisquet::BlockCheck::kInconsistent);
  block[3] = 0x08;
  block[4] = 0x10;
  CHECK(frisquet::check_block_against_broadcast(block, 0x14, 8.0f, 3, 0x22, 0x50) ==
        frisquet::BlockCheck::kConsistent);
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

// Pairing: synthetic frames below; the real exchange of 28/10 is checked first.
static void test_pairing() {
  using namespace frisquet;
  const uint8_t home[4] = {0x05, 0xD9, 0x8D, 0xC9};
  {
    const Bytes &real_offer = F["payload_association_offer"];
    const Bytes &real_confirm = F["payload_association_confirm"];
    AssociationOffer r;
    CHECK(decode_association_offer(real_offer.data(), real_offer.size(), r));
    CHECK(r.destination == 0x00 && r.association == 0xD5 && r.message_id == 0xD4 && r.reception == 0x02);
    CHECK(memcmp(r.network, home, 4) == 0);
    uint8_t built[kAssociationConfirmLength];
    CHECK(build_association_confirm(real_offer.data(), real_offer.size(), kIdConnect, built, sizeof(built)) == 10);
    CHECK(Bytes(built, built + 10) == real_confirm);
    TxPolicy p;
    p.level = TxLevel::kRead;
    p.dry_run = false;
    p.association = 0xE3;  // the previous association: the confirmation carries the offer's byte
    p.pairing = true;
    CHECK(evaluate_tx(p, real_confirm.data(), real_confirm.size(), real_offer.data(), real_offer.size()) ==
          TxVerdict::kSend);
    PairingSession s;
    CHECK(s.start(PairingKind::kAssociate, kIdConnect, home, 0));
    CHECK(s.on_frame(100, real_offer.data(), real_offer.size()));
    size_t n = 0;
    uint8_t out[16];
    for (uint32_t t = 100; s.active(); t += 30) s.poll(t, out, n);
    CHECK(s.state() == PairingState::kSucceeded && s.association() == 0xD5 && memcmp(s.network(), home, 4) == 0);
  }
  const Bytes offer = {0xFF, 0x80, 0xE7, 0x2A, 0x01, 0x41, 0x04, 0x12, 0x34, 0x56, 0x78};
  const uint8_t learned[4] = {0x12, 0x34, 0x56, 0x78};

  AssociationOffer o;
  CHECK(decode_association_offer(offer.data(), offer.size(), o));
  CHECK(o.association == 0xE7 && o.message_id == 0x2A && o.reception == 0x01);
  CHECK(memcmp(o.network, learned, 4) == 0);
  {
    Bytes bad = offer;
    bad[1] = 0x08;  // not from the boiler
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    bad = offer;
    bad[4] = 0x81;  // an answer, not an offer
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    bad = offer;
    bad[6] = 0x05;  // length byte
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    bad = offer;
    memset(bad.data() + 7, 0xFF, 4);  // the broadcast network is never an answer
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    bad = offer;
    memset(bad.data() + 7, 0x00, 4);
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    bad = offer;
    bad[2] = 0xFF;  // "not associated"
    CHECK(!decode_association_offer(bad.data(), bad.size(), o));
    CHECK(!decode_association_offer(offer.data(), offer.size() - 1, o));
  }

  uint8_t confirm[kAssociationConfirmLength];
  CHECK(build_association_confirm(offer.data(), offer.size(), kIdConnect, confirm, sizeof(confirm)) == 10);
  const Bytes expected_confirm = {0x80, 0x7E, 0xE7, 0x2A, 0x81, 0x41, 0x12, 0x34, 0x56, 0x78};
  CHECK(Bytes(confirm, confirm + 10) == expected_confirm);
  CHECK(build_association_confirm(offer.data(), offer.size(), kIdConnect, confirm, 9) == 0);

  uint8_t out[16];
  size_t length = 0;

  // NetworkID recovery: broadcast while listening, the learned network afterwards, nothing transmitted.
  {
    PairingSession s;
    CHECK(!s.start(PairingKind::kRecoverNetwork, kIdConnect, kBroadcastNetwork, 0));
    CHECK(s.start(PairingKind::kRecoverNetwork, kIdConnect, home, 1000));
    CHECK(!s.start(PairingKind::kRecoverNetwork, kIdConnect, home, 1000));  // one session at a time
    CHECK(memcmp(s.network(), kBroadcastNetwork, 4) == 0);
    const Bytes read_answer = {0x7E, 0x80, 0xE3, 0x13, 0x81, 0x03, 0x02, 0x00, 0x90};
    CHECK(!s.on_frame(2000, read_answer.data(), read_answer.size()));
    CHECK(s.on_frame(3000, offer.data(), offer.size()));
    CHECK(s.state() == PairingState::kSucceeded);
    CHECK(memcmp(s.network(), learned, 4) == 0);
    CHECK(!s.poll(3001, out, length) && length == 0);
    s.acknowledge();
    CHECK(s.state() == PairingState::kIdle && memcmp(s.network(), learned, 4) == 0);
  }
  // Regression of the bug fixed in the MQTT firmware on 25/10: a failed listen must not stay on broadcast.
  {
    PairingSession s;
    CHECK(s.start(PairingKind::kRecoverNetwork, kIdConnect, home, 1000));
    CHECK(!s.poll(1000 + kPairingListenMs - 1, out, length));
    CHECK(s.active());
    CHECK(!s.poll(1000 + kPairingListenMs, out, length));
    CHECK(s.state() == PairingState::kFailed);
    CHECK(memcmp(s.network(), home, 4) == 0);
    CHECK(!s.on_frame(1000 + kPairingListenMs + 1, offer.data(), offer.size()));  // too late
    CHECK(memcmp(s.network(), home, 4) == 0);
  }
  {
    PairingSession s;
    CHECK(s.start(PairingKind::kRecoverNetwork, kIdConnect, home, 0xFFFFF000u));  // across the millis() wrap
    CHECK(!s.poll(0x00000100u, out, length) && s.active());
    s.cancel();
    CHECK(s.state() == PairingState::kCancelled && memcmp(s.network(), home, 4) == 0);
  }

  // Association: confirm 10 times, 30 ms apart, still on broadcast; then the new network and association.
  {
    PairingSession s;
    CHECK(s.start(PairingKind::kAssociate, kIdConnect, home, 0));
    CHECK(s.on_frame(5000, offer.data(), offer.size()));
    CHECK(s.state() == PairingState::kConfirming);
    CHECK(memcmp(s.network(), kBroadcastNetwork, 4) == 0);
    int sent = 0;
    uint32_t t = 5000;
    for (int i = 0; i < 40 && s.active(); ++i, t += 10) {
      if (s.poll(t, out, length)) {
        ++sent;
        CHECK(length == 10 && Bytes(out, out + 10) == expected_confirm);
        CHECK(!s.poll(t, out, length));  // never two in the same instant
        if (s.active()) CHECK(memcmp(s.network(), kBroadcastNetwork, 4) == 0);
      }
    }
    CHECK(sent == kAssociationConfirmRepeats);
    CHECK(s.state() == PairingState::kSucceeded);
    CHECK(memcmp(s.network(), learned, 4) == 0 && s.association() == 0xE7);
    CHECK(s.offer_length() == offer.size());
  }
  // A radio that cannot confirm (emergency stop, nothing polled) gives the network back.
  {
    PairingSession s;
    CHECK(s.start(PairingKind::kAssociate, kIdConnect, home, 0));
    CHECK(s.on_frame(5000, offer.data(), offer.size()));
    CHECK(!s.poll(10000, out, length));
    CHECK(s.state() == PairingState::kFailed && memcmp(s.network(), home, 4) == 0);
  }
  {
    PairingSession s;
    CHECK(s.start(PairingKind::kAssociate, kIdConnect, home, 0));
    CHECK(s.on_frame(5000, offer.data(), offer.size()));
    CHECK(s.poll(5000, out, length));
    s.cancel();
    CHECK(s.state() == PairingState::kCancelled && memcmp(s.network(), home, 4) == 0);
    CHECK(!s.poll(5100, out, length));
  }

  // Gate: only the exact confirmation of the received offer, only when pairing is allowed.
  {
    TxPolicy p;
    p.level = TxLevel::kRead;
    p.dry_run = false;
    p.association = 0xE3;
    TxClass cls;
    CHECK(evaluate_tx(p, confirm, 10, offer.data(), offer.size(), &cls) == TxVerdict::kBlockedLevel);
    CHECK(cls == TxClass::kAssociationConfirm);
    p.pairing = true;
    CHECK(evaluate_tx(p, confirm, 10, offer.data(), offer.size()) == TxVerdict::kSend);
    CHECK(evaluate_tx(p, confirm, 10) == TxVerdict::kBlockedAddress);
    Bytes other = offer;
    other[10] = 0x79;
    CHECK(evaluate_tx(p, confirm, 10, other.data(), other.size()) == TxVerdict::kBlockedAddress);
    Bytes forged(confirm, confirm + 10);
    forged[1] = 0x08;  // pretending to be the satellite
    CHECK(evaluate_tx(p, forged.data(), forged.size(), offer.data(), offer.size()) == TxVerdict::kBlockedIdentity);
    forged.assign(confirm, confirm + 10);
    forged[4] = 0x01;  // not an answer
    CHECK(evaluate_tx(p, forged.data(), forged.size(), offer.data(), offer.size()) == TxVerdict::kMalformed);
    CHECK(evaluate_tx(p, offer.data(), offer.size(), offer.data(), offer.size()) == TxVerdict::kBlockedIdentity ||
          evaluate_tx(p, offer.data(), offer.size(), offer.data(), offer.size()) == TxVerdict::kMalformed);
    p.dry_run = true;
    CHECK(evaluate_tx(p, confirm, 10, offer.data(), offer.size()) == TxVerdict::kDryRun);
    p.kill_switch = true;
    CHECK(evaluate_tx(p, confirm, 10, offer.data(), offer.size()) == TxVerdict::kBlockedKillSwitch);
  }

  // Persistence of the learned identity.
  {
    uint8_t blob[kNetworkStoreSize];
    pack_network_store(learned, 0xE7, blob);
    uint8_t network[4] = {};
    uint8_t association = 0;
    CHECK(unpack_network_store(blob, sizeof(blob), network, association));
    CHECK(memcmp(network, learned, 4) == 0 && association == 0xE7);
    pack_network_store(home, 0xFF, blob);
    CHECK(unpack_network_store(blob, sizeof(blob), network, association) && association == 0xFF);
    blob[6] ^= 0x01;
    CHECK(!unpack_network_store(blob, sizeof(blob), network, association));
    pack_network_store(kBroadcastNetwork, 0xE7, blob);
    CHECK(!unpack_network_store(blob, sizeof(blob), network, association));
    CHECK(!unpack_network_store(blob, sizeof(blob) - 1, network, association));
  }
}

// Burner detection from the live heating-body temperature (0xA05E), replayed from the 2026-10-31 hot-water draw:
// flame 10:54:35 - 10:56:30 (user), water opened ~10:53.
static void test_burner() {
  using namespace frisquet;
  // Boiler push (panel change, 01/11 12:42): acknowledged at once with the address and size echoed.
  {
    const Bytes &push = F["payload_boiler_push_a0f0"];
    CHECK(push.size() == 37 && is_boiler_push(push.data(), push.size(), kIdConnect));
    CHECK(!is_boiler_push(push.data(), push.size(), 0x08));
    uint8_t ack[16];
    CHECK(build_push_ack(push.data(), push.size(), kIdConnect, ack, sizeof(ack)) == 10);
    const Bytes expected = {0x80, 0x7E, 0xD5, 0x68, 0x85, 0x10, 0xA0, 0xF0, 0x00, 0x0D};
    CHECK(Bytes(ack, ack + 10) == expected);
    Bytes event = {0x7E, 0x80, 0xD5, 0x21, 0x05, 0x45};
    event.resize(27, 0x00);
    CHECK(build_push_ack(event.data(), event.size(), kIdConnect, ack, sizeof(ack)) == 6);
    CHECK(ack[4] == 0x85 && ack[5] == 0x45 && ack[3] == 0x21);
    TxPolicy p;
    p.level = TxLevel::kAck;
    p.dry_run = false;
    p.association = 0xD5;
    TxClass cls;
    CHECK(evaluate_tx(p, expected.data(), expected.size(), push.data(), push.size(), &cls) == TxVerdict::kSend);
    CHECK(cls == TxClass::kPushAck);
    CHECK(evaluate_tx(p, expected.data(), expected.size()) == TxVerdict::kBlockedAddress);  // no push to answer
    Bytes forged = expected;
    forged[7] = 0xFC;
    CHECK(evaluate_tx(p, forged.data(), forged.size(), push.data(), push.size()) == TxVerdict::kBlockedAddress);
    p.level = TxLevel::kRead;
    CHECK(evaluate_tx(p, expected.data(), expected.size(), push.data(), push.size()) == TxVerdict::kBlockedLevel);

    ConnectEngine e;
    e.set_policy(engine_policy(TxLevel::kAck, false));
    TxPolicy ep = e.policy();
    ep.association = 0xD5;
    e.set_policy(ep);
    e.clock().set_from_bcd(0, 0x26, 0x11, 0x01, 0x12, 0x40, 0x05);  // quiet window: acknowledgements still go
    ReadResult result;
    CHECK(!e.on_frame(100, push.data(), push.size(), result));
    TxRequest tx;
    CHECK(e.poll(110, tx) && tx.kind == TxClass::kPushAck && tx.verdict == TxVerdict::kSend);
    CHECK(tx.length == 10 && Bytes(tx.frame, tx.frame + 10) == expected);
    CHECK(e.stats().pushes == 1);
    CHECK(e.hot_water_mode_known() && e.hot_water_mode() == HotWaterMode::kEcoScheduled);  // 0x90 at A0FC
    CHECK(!e.poll(120, tx));
    // The satellite's own consigne exchange has the format our direct consigne uses.
    const Bytes &sat = F["payload_satellite_consigne"];
    uint8_t ours[32];
    CHECK(build_satellite_consigne(ours, sizeof(ours), kIdZone1, 0xFA, sat[3], sat.data() + 15) == sat.size());
    CHECK(Bytes(ours, ours + sat.size()) == sat);
  }
  // Consigne exchange in the name of the zone 1 satellite (31/10 test of direct setpoints).
  {
    const uint8_t data[8] = {0x00, 0xB2, 0x00, 0x55, 0x00, 0x10, 0x00, 0xC6};  // ambient 17.8, setpoint 8.5, Hors gel
    uint8_t frame[32];
    CHECK(build_satellite_consigne(frame, sizeof(frame), kIdZone1, 0xFA, 0x42, data) == 23);
    const Bytes expected = {0x80, 0x08, 0xFA, 0x42, 0x01, 0x17, 0xA0, 0x29, 0x00, 0x15, 0xA0, 0x2F,
                            0x00, 0x04, 0x08, 0x00, 0xB2, 0x00, 0x55, 0x00, 0x10, 0x00, 0xC6};
    CHECK(Bytes(frame, frame + 23) == expected);
    CHECK(satellite_consigne_address(kIdZone2) == 0xA034);
    TxPolicy p;
    p.level = TxLevel::kWrite;
    p.dry_run = false;
    p.association = 0xD5;
    TxClass cls;
    CHECK(evaluate_tx(p, frame, 23, nullptr, 0, &cls) == TxVerdict::kBlockedIdentity);  // no satellite association
    CHECK(cls == TxClass::kSatelliteConsigne);
    p.satellite_association = 0xFA;
    CHECK(evaluate_tx(p, frame, 23) == TxVerdict::kBlockedLevel);  // override not allowed
    p.satellite_override = true;
    CHECK(evaluate_tx(p, frame, 23) == TxVerdict::kSend);
    Bytes bad(frame, frame + 23);
    bad[18] = 0x56;  // 8.6 C: not a 0.5 step
    CHECK(evaluate_tx(p, bad.data(), bad.size()) == TxVerdict::kBlockedAddress);
    bad.assign(frame, frame + 23);
    bad[2] = 0xFB;  // another association
    CHECK(evaluate_tx(p, bad.data(), bad.size()) == TxVerdict::kBlockedIdentity);
    bad.assign(frame, frame + 23);
    bad[11] = 0x34;  // zone 2 address from zone 1
    CHECK(evaluate_tx(p, bad.data(), bad.size()) == TxVerdict::kBlockedAddress);

    ConnectEngine e;
    TxPolicy ep = engine_policy(TxLevel::kWrite, false);
    e.set_policy(ep);
    CHECK(!e.request_satellite_consigne(data));
    ep.satellite_override = true;
    ep.satellite_association = 0xFA;
    e.set_policy(ep);
    e.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x12, 0x57, 0x15);
    CHECK(e.request_satellite_consigne(data) && !e.request_satellite_consigne(data));
    TxRequest tx;
    CHECK(e.poll(100, tx) && tx.kind == TxClass::kSatelliteConsigne && tx.verdict == TxVerdict::kSend);
    CHECK(tx.frame[1] == 0x08 && tx.frame[2] == 0xFA && memcmp(tx.frame + 15, data, 8) == 0);
    Bytes answer = F["payload_sat_broadcast_reduced"];  // 08 80 FA <id> 81 17 2A ...
    answer[3] = tx.frame[3];
    ReadResult result;
    CHECK(e.on_frame(200, answer.data(), answer.size(), result) && result.kind == ReadKind::kSatelliteConsigne);
    CHECK(!e.reading());
  }

  // The satellite's block write-back, in its name (SATZ): only the mode may change, answered with reception FE.
  {
    const Bytes &known = F["payload_zone_relay_live"];  // mode 05, options 21
    uint8_t frame[64];
    CHECK(build_satellite_block(frame, sizeof(frame), kIdZone1, 0xFA, 0xA0, kIdConnect, known.data() + 15) == 63);
    const Bytes head = {0x80, 0x08, 0xFA, 0xA0, 0x7E, 0x17, 0xA1, 0x54, 0x00, 0x15, 0xA1, 0x54, 0x00, 0x18, 0x30};
    CHECK(Bytes(frame, frame + 15) == head);  // as the satellite's own, 01/11/2026 13:29:25
    TxPolicy p = engine_policy(TxLevel::kWrite, false);
    p.satellite_association = 0xFA;
    TxClass cls;
    CHECK(evaluate_tx(p, frame, 63, nullptr, 0, &cls, known.data() + 15) == TxVerdict::kBlockedLevel);
    CHECK(cls == TxClass::kSatelliteBlock);
    p.satellite_override = true;
    CHECK(evaluate_tx(p, frame, 63, nullptr, 0, nullptr, known.data() + 15) == TxVerdict::kSend);
    CHECK(evaluate_tx(p, frame, 63) == TxVerdict::kBlockedAddress);  // no known block
    Bytes bad(frame, frame + 63);
    bad[15] = 0x96;  // comfort changed: only the mode may differ
    CHECK(evaluate_tx(p, bad.data(), 63, nullptr, 0, nullptr, known.data() + 15) == TxVerdict::kBlockedAddress);
    bad.assign(frame, frame + 63);
    bad[4] = 0x01;  // not to the Connect
    CHECK(evaluate_tx(p, bad.data(), 63, nullptr, 0, nullptr, known.data() + 15) == TxVerdict::kBlockedIdentity);
    bad.assign(frame, frame + 63);
    bad[18] = 0x06;
    bad[19] = 0x30;  // Confort with the frost bit: incoherent
    CHECK(evaluate_tx(p, bad.data(), 63, nullptr, 0, nullptr, known.data() + 15) == TxVerdict::kBlockedAddress);

    ConnectEngine s;
    s.set_policy(p);
    s.set_next_message_id(0x50);
    s.clock().set_from_bcd(0, 0x26, 0x11, 0x01, 0x13, 0x45, 0x00);
    CHECK(!s.request_satellite_block(0x06));  // block unknown
    ReadResult result;
    s.on_frame(100, known.data(), known.size(), result);
    TxRequest tx;
    CHECK(s.poll(110, tx) && tx.kind == TxClass::kZoneAck);
    CHECK(!s.request_satellite_block(0x09));
    CHECK(s.request_satellite_block(0x06) && !s.request_satellite_block(0x06));
    CHECK(s.poll(200, tx) && tx.kind == TxClass::kSatelliteBlock && tx.verdict == TxVerdict::kSend);
    CHECK(tx.frame[1] == 0x08 && tx.frame[4] == 0x7E && tx.frame[18] == 0x06 && tx.frame[19] == 0x21);
    // The boiler forwards it to the Connect (a relay, acknowledged), then answers the "satellite".
    Bytes forward(63);
    const Bytes fhead = {0x7E, 0x80, 0xE3, tx.frame[3], 0x08, 0x17, 0xA1, 0x54, 0x00, 0x15, 0xA1, 0x54, 0x00, 0x18, 0x30};
    memcpy(forward.data(), fhead.data(), 15);
    memcpy(forward.data() + 15, tx.frame + 15, 48);
    CHECK(!s.on_frame(300, forward.data(), forward.size(), result));
    TxRequest ack;
    CHECK(s.poll(310, ack) && ack.kind == TxClass::kZoneAck && ack.verdict == TxVerdict::kSend);
    CHECK(s.zone_block(0)[3] == 0x06);
    Bytes reply(55);
    const Bytes rhead = {0x08, 0x80, 0xFA, tx.frame[3], 0xFE, 0x17, 0x30};
    memcpy(reply.data(), rhead.data(), 7);
    memcpy(reply.data() + 7, tx.frame + 15, 48);
    CHECK(s.on_frame(400, reply.data(), reply.size(), result) && result.kind == ReadKind::kSatelliteBlock);
    CHECK(!s.reading());
  }

  // Dual path: a zone write also sends the consigne directly, re-asserted after the satellite's own exchange.
  {
    const Bytes &known = F["payload_zone_relay_live"];  // mode 05, options 21, comfort 19.0
    ConnectEngine d;
    TxPolicy dp = engine_policy(TxLevel::kWrite, false);
    dp.zone_write_mask = kZoneFieldSetpoints | kZoneFieldMode | kZoneFieldProgram;
    dp.satellite_override = true;
    dp.satellite_association = 0xFA;
    d.set_policy(dp);
    d.set_next_message_id(0x40);
    d.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x22, 0x14, 0x30);  // Sunday 25/10 22:14:30
    ReadResult result;
    d.on_frame(100, known.data(), known.size(), result);
    TxRequest tx;
    CHECK(d.poll(110, tx) && tx.kind == TxClass::kZoneAck);
    // The state page (broadcast of 22:10:12, weekday 0): Auto reduced, setpoint 17.0.
    Bytes page = F["payload_sat_broadcast_reduced"];
    d.on_frame(200, page.data(), page.size(), result);
    // Hors gel: the boiler gets frost 8.0 at once, then the block goes to the satellite.
    CHECK(d.request_zone_mode(0, 0x08));
    CHECK(d.poll(300, tx) && tx.kind == TxClass::kSatelliteConsigne && tx.verdict == TxVerdict::kSend);
    CHECK(be16(tx.frame + 17) == 80 && tx.frame[20] == 0x10);       // setpoint 8.0, mode byte Hors gel
    CHECK(tx.frame[15] == page[7 + 12] && tx.frame[16] == page[7 + 13]);  // ambient copied
    Bytes reply = page;
    reply[3] = tx.frame[3];
    reply[7 + 14] = 0x00;
    reply[7 + 15] = 0x50;  // the page now shows 8.0
    reply[7 + 17] = 0x10;
    d.on_frame(400, reply.data(), reply.size(), result);
    bool block = false;
    for (uint32_t t = 500; t < 4000; t += 100)
      if (d.poll(t, tx) && tx.kind == TxClass::kZoneWrite) block = true;
    CHECK(block && d.direct_active());
    // The satellite's own regular exchange puts its old setpoint back: re-asserted (after the burst in progress).
    Bytes own = page;
    own[3] = 0x77;
    d.on_frame(5000, own.data(), own.size(), result);
    bool again = false;
    uint32_t t = 5000;
    for (; t < 30000 && !again; t += 100)
      if (d.poll(t, tx) && tx.kind == TxClass::kSatelliteConsigne) again = be16(tx.frame + 17) == 80;
    CHECK(again);
    Bytes reply2 = reply;
    reply2[3] = tx.frame[3];
    d.on_frame(t, reply2.data(), reply2.size(), result);  // the boiler answers the re-assertion
    // An exchange that already shows our setpoint changes nothing.
    Bytes ok = reply;
    ok[3] = 0x78;
    d.on_frame(t, ok.data(), ok.size(), result);
    bool extra = false;
    for (const uint32_t end = t + 30000; t < end; t += 100)
      if (d.poll(t, tx) && tx.kind == TxClass::kSatelliteConsigne) extra = true;
    CHECK(!extra);
    // A change on the satellite meanwhile supersedes the write: no more direct consigne.
    Bytes local = known;
    local[15] = 0x82;
    local[3] = 0x50;
    d.on_frame(t, local.data(), local.size(), result);
    CHECK(d.zone_write_state() == WriteState::kSuperseded);
    for (const uint32_t end = t + 1000; t < end; t += 100) d.poll(t, tx);
    CHECK(!d.direct_active());
    d.on_frame(t, own.data(), own.size(), result);
    extra = false;
    for (const uint32_t end = t + 30000; t < end; t += 100)
      if (d.poll(t, tx) && tx.kind == TxClass::kSatelliteConsigne) extra = true;
    CHECK(!extra);
  }
  // Timing (01/11 12:31: a request in the quiet window waited 71 s): the consigne goes even in the quiet window,
  // except within 20 s of the zone 1 satellite's own exchange (:x0:08).
  {
    CHECK(near_zone1_exchange(0) && near_zone1_exchange(27) && !near_zone1_exchange(28));
    CHECK(near_zone1_exchange(588) && !near_zone1_exchange(587) && !near_zone1_exchange(73));
    const uint8_t data[8] = {0x00, 0xA9, 0x00, 0x50, 0x00, 0x10, 0x00, 0xC6};
    TxPolicy qp = engine_policy(TxLevel::kWrite, false);
    qp.satellite_override = true;
    qp.satellite_association = 0xFA;
    TxRequest tx;
    ConnectEngine q1;
    q1.set_policy(qp);
    q1.clock().set_from_bcd(0, 0x26, 0x11, 0x01, 0x12, 0x31, 0x13);  // :x1:13, quiet window
    CHECK(q1.request_satellite_consigne(data));
    CHECK(q1.poll(100, tx) && tx.kind == TxClass::kSatelliteConsigne);
    ConnectEngine q2;
    q2.set_policy(qp);
    q2.clock().set_from_bcd(0, 0x26, 0x11, 0x01, 0x12, 0x30, 0x05);  // :x0:05, the satellite's exchange
    CHECK(q2.request_satellite_consigne(data));
    CHECK(!q2.poll(100, tx));
    CHECK(q2.poll(24000, tx) && tx.kind == TxClass::kSatelliteConsigne);  // :x0:29
  }
  // Regression 31/10 12:37: the answer to our own consigne must not discard a restored block.
  {
    ConnectEngine r;
    TxPolicy rp = engine_policy(TxLevel::kWrite, false);
    rp.satellite_override = true;
    rp.satellite_association = 0xFA;
    r.set_policy(rp);
    r.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x22, 0x14, 0x30);
    const Bytes &known = F["payload_zone_relay_live"];  // comfort 19.0, mode 05
    r.restore_zone_block(0, known.data() + 15);
    CHECK(r.zone_block_known(0) && !r.zone_block_confirmed(0));
    uint8_t data[8] = {0x00, 0xA9, 0x00, 0x55, 0x00, 0x10, 0x00, 0xC6};  // 8.5, Hors gel: not this block
    CHECK(r.request_satellite_consigne(data));
    TxRequest tx;
    CHECK(r.poll(100, tx) && tx.kind == TxClass::kSatelliteConsigne);
    Bytes answer = F["payload_sat_broadcast_reduced"];
    answer[3] = tx.frame[3];
    answer[7 + 14] = 0x00;
    answer[7 + 15] = 0x55;
    answer[7 + 17] = 0x10;
    ReadResult result;
    r.on_frame(200, answer.data(), answer.size(), result);
    SatelliteBroadcast b;
    CHECK(decode_satellite_broadcast(answer.data(), answer.size(), b));
    r.on_zone_broadcast(0, b.mode_raw[0], b.setpoint[0], b.weekday, b.hour, b.minute);  // as the component does
    CHECK(r.zone_block_known(0) && !r.zone_block_confirmed(0));
    // The satellite's own inconsistent exchange still discards it.
    Bytes own = answer;
    own[3] = static_cast<uint8_t>(tx.frame[3] + 7);
    r.on_frame(300, own.data(), own.size(), result);
    r.on_zone_broadcast(0, b.mode_raw[0], b.setpoint[0], b.weekday, b.hour, b.minute);
    CHECK(!r.zone_block_known(0));
  }
  // Auto: the active setpoint follows the programme at the boiler's time.
  {
    const Bytes &known = F["payload_zone_relay_live"];
    ConnectEngine a;
    TxPolicy ap = engine_policy(TxLevel::kWrite, false);
    ap.zone_write_mask = kZoneFieldSetpoints | kZoneFieldMode | kZoneFieldProgram;
    ap.satellite_override = true;
    ap.satellite_association = 0xFA;
    a.set_policy(ap);
    a.clock().set_from_bcd(0, 0x26, 0x10, 0x25, 0x22, 0x14, 0x30);
    ReadResult result;
    a.on_frame(100, known.data(), known.size(), result);
    TxRequest tx;
    a.poll(110, tx);
    Bytes page = F["payload_sat_broadcast_reduced"];
    page[7 + 15] = 0x50;  // pretend the boiler holds 8.0 so that a change is needed
    a.on_frame(200, page.data(), page.size(), result);
    CHECK(a.request_zone_setpoints(0, 0x96, 0x78, 0x1E));  // still Auto; Sunday 22:14 is reduced in this programme
    CHECK(a.poll(300, tx) && tx.kind == TxClass::kSatelliteConsigne);
    const uint8_t *prog = known.data() + 15 + 6;
    const bool comfort_now = (prog[0 * 6 + 44 / 8] >> (44 % 8)) & 1;  // Sunday, slot 44 = 22:00-22:30
    CHECK(be16(tx.frame + 17) == (comfort_now ? 0x96 + 50 : 0x78 + 50));
    CHECK(tx.frame[20] == (comfort_now ? 0x21 : 0x20));
  }
  float c = 0;
  const Bytes answer = {0x7E, 0x80, 0xD5, 0x10, 0x81, 0x03, 0x02, 0x03, 0x2E};
  CHECK(decode_body_live(answer.data(), answer.size(), c) && fabsf(c - 81.4f) < 0.01f);
  const Bytes wrong = {0x7E, 0x80, 0xD5, 0x10, 0x81, 0x03, 0x04, 0x03, 0x2E, 0x00, 0x00};
  CHECK(!decode_body_live(wrong.data(), wrong.size(), c));
  CHECK(classify_read_address(0xA05E) == ReadKind::kBody);

  struct Sample { int s; float t; } samples[] = {
      {33, 81.4f}, {44, 81.4f}, {55, 81.4f}, {69, 79.8f}, {85, 78.7f}, {99, 77.2f}, {113, 75.7f}, {127, 74.2f},
      {138, 72.7f}, {152, 70.8f}, {165, 68.9f}, {180, 67.6f}, {194, 68.0f}, {215, 70.8f}, {226, 72.7f}, {237, 75.2f},
      {251, 78.2f}, {262, 80.3f}, {273, 81.9f}, {284, 84.2f}, {296, 85.3f}, {311, 84.2f}, {325, 83.0f}, {342, 81.9f},
      {353, 80.9f}, {365, 79.8f}, {379, 79.3f}, {390, 78.7f}, {403, 78.2f}, {417, 78.2f}};
  BurnerDetector d;
  int first_on = -1, first_off_after = -1;
  for (const Sample &x : samples) {  // seconds after 10:52:00
    const BurnerState st = d.feed(static_cast<uint32_t>(x.s) * 1000, x.t);
    if (x.s == 33) CHECK(st == BurnerState::kUnknown);
    if (st == BurnerState::kOn && first_on < 0) first_on = x.s;
    if (first_on >= 0 && st == BurnerState::kOff && first_off_after < 0) first_off_after = x.s;
  }
  // Flame at 155 s (10:54:35): seen at 215 s, within the thermal lag; stop at 270 s: seen at 311 s.
  CHECK(first_on == 215);
  CHECK(first_off_after == 311);
  CHECK(d.state() == BurnerState::kOff);

  // A gap longer than 45 s (satellites' window) only sets a new reference, unless the temperature fell.
  BurnerDetector g;
  g.feed(0, 60.0f);
  g.feed(15000, 60.0f);
  CHECK(g.state() == BurnerState::kOff);
  CHECK(g.feed(315000, 75.0f) == BurnerState::kOff);  // +15 C after 5 min: no conclusion
  CHECK(g.feed(330000, 76.5f) == BurnerState::kOn);
  CHECK(g.feed(345000, NAN) == BurnerState::kOn);
  CHECK(g.feed(645000, 70.0f) == BurnerState::kOff);  // lower after the gap: off

  // Second validation (31/10 11:08:20 - 11:09:45): on at the first big rise, a +0.6 C step at rest is noise.
  BurnerDetector v;
  const float rest[] = {67.6f, 65.7f, 64.8f, 66.6f};
  uint32_t t = 0;
  for (float x : rest) v.feed(t += 15000, x);
  CHECK(v.state() == BurnerState::kOn);  // 64.8 -> 66.6
  BurnerDetector n;
  n.feed(0, 83.0f);
  n.feed(15000, 83.0f);
  CHECK(n.feed(30000, 83.6f) == BurnerState::kOff);
  CHECK(n.feed(45000, 83.6f) == BurnerState::kOff);

  // Heating (31/10 11:49 - 12:09, flame continuous from ~11:55): the body swings, the flue snapshots decide.
  {
    BurnerFusion f;
    const float swings[] = {82.5f, 78.7f, 67.6f, 53.0f, 44.7f, 41.1f, 43.8f, 48.3f, 53.9f, 59.5f, 64.8f, 69.4f, 72.2f};
    uint32_t ft = 0;
    f.set_heating_demand(true);
    f.feed_flue(0.0f);  // 11:52:55 snapshot, before the flame
    for (float x : swings) f.feed_body(ft += 15000, x);
    CHECK(f.state() == BurnerState::kOff);  // the slope would say on/off; the snapshot rules
    f.feed_flue(122.0f);  // 11:57:55
    for (float x : swings) {
      f.feed_body(ft += 15000, x);
      CHECK(f.state() == BurnerState::kOn);
    }
    // Demand over: the slope takes over (hot water).
    f.set_heating_demand(false);
    f.feed_body(ft += 15000, 72.0f);
    f.feed_body(ft += 15000, 72.0f);
    CHECK(f.state() == BurnerState::kOff);
    f.feed_body(ft += 15000, 74.0f);
    CHECK(f.state() == BurnerState::kOn);
    BurnerFusion g2;
    g2.set_heating_demand(true);
    CHECK(g2.state() == BurnerState::kUnknown);  // no snapshot yet
    g2.feed_flue(NAN);
    CHECK(g2.state() == BurnerState::kUnknown);
  }
  {
    float flue = 0;
    const Bytes answer = {0x7E, 0x80, 0xD5, 0x10, 0x81, 0x03, 0x02, 0x09, 0x88};  // 0x0988 / 20
    CHECK(decode_flue_snapshot(answer.data(), answer.size(), flue) && fabsf(flue - 122.0f) < 0.01f);
    CHECK(classify_read_address(0x79E6) == ReadKind::kFlue);
  }

  // Engine: one 0xA05E read every 15 s with the policy flag, none in the satellites' window.
  TxRequest tx;
  ConnectEngine e;
  TxPolicy p = engine_policy(TxLevel::kRead, true);
  p.live_body_reads = true;
  e.set_policy(p);
  e.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x57, 0x15);  // 7:15 into the cycle: no other slot
  int reads = 0;
  for (uint32_t t = 0; t < 40000; t += 500)
    if (e.poll(t, tx)) {
      CHECK(be16(tx.frame + 6) == 0xA05E && be16(tx.frame + 8) == 0x01);
      ++reads;
    }
  CHECK(reads == 3);  // 0, 15, 30 s (the :08 slot starts at 45 s)
  // 0.7.20: in the satellites' window the live read still goes (a hot-water burn there was missed), only the 40 s around
  // the zone 1 satellite's exchange are avoided, and nothing else is read.
  ConnectEngine q;
  q.set_policy(p);
  q.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x50, 0x30);  // 0:30, quiet until 2:30
  int quiet_reads = 0;
  for (uint32_t t = 0; t < 60000; t += 500)
    if (q.poll(t, tx)) {
      CHECK(be16(tx.frame + 6) == 0xA05E);
      ++quiet_reads;
    }
  CHECK(quiet_reads == 4);  // 0, 15, 30, 45 s
  ConnectEngine nz;
  nz.set_policy(p);
  nz.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x59, 0x50);  // 9:50: around the satellite's exchange
  CHECK(!nz.poll(0, tx) && !nz.poll(30000, tx));                      // up to 0:20
  CHECK(nz.poll(38500, tx) && be16(tx.frame + 6) == 0xA05E);         // 0:28.5
  ConnectEngine off;
  off.set_policy(engine_policy(TxLevel::kRead, true));
  off.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x57, 0x15);
  CHECK(!off.poll(0, tx));  // flag off: no live reads

  // At :03, the second flue snapshot (0x79E6) follows the 0x79F2 read when live reads are on.
  {
    ConnectEngine s3;
    s3.set_policy(p);
    s3.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x52, 0x59);  // 2:59
    bool flue = false;
    for (uint32_t t = 0; t < 8000; t += 100)
      if (s3.poll(t, tx) && be16(tx.frame + 6) == 0x79E6) flue = be16(tx.frame + 8) == 0x01;
    CHECK(flue);
    ConnectEngine s0;
    s0.set_policy(engine_policy(TxLevel::kRead, true));
    s0.clock().set_from_bcd(0, 0x26, 0x10, 0x31, 0x10, 0x52, 0x59);
    flue = false;
    for (uint32_t t = 0; t < 8000; t += 100)
      if (s0.poll(t, tx) && be16(tx.frame + 6) == 0x79E6) flue = true;
    CHECK(!flue);
  }

  // The boiler clock is learnt from the first 0xA029 answer, without waiting for a broadcast.
  ConnectEngine ce;
  ce.set_policy(engine_policy(TxLevel::kRead, false));
  ReadResult result;
  CHECK(!ce.clock().known());
  CHECK(ce.poll(180000, tx) && be16(tx.frame + 6) == 0x79F2);  // uptime-based :03 slot
  ce.on_frame(180050, with_id(F["payload_79f2_idle"], tx.frame[3]).data(), F["payload_79f2_idle"].size(), result);
  CHECK(ce.poll(180100, tx) && be16(tx.frame + 6) == 0xA029);
  Bytes status = F["payload_sat_broadcast_reduced"];  // 2026-10-25 22:10:12
  const Bytes header = {0x7E, 0x80, 0xE3, tx.frame[3], 0x81, 0x03};
  std::copy(header.begin(), header.end(), status.begin());
  CHECK(ce.on_frame(180200, status.data(), status.size(), result) && result.kind == ReadKind::kStatus);
  CHECK(ce.clock().known() && ce.clock().seconds(180200) % 600 == 12);  // 22:10:12 -> 12 s into the cycle
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
  test_hot_water_write_frame_and_gate();
  test_engine_hot_water_write();
  test_zone_write();
  test_zone_mode_and_program_write();
  test_program_text();
  test_zone_store_and_restore();
  test_zone_block_and_fault_broadcast();
  test_activity();
  test_other_reads();
  test_read_tracker();
  test_slot_schedule();
  test_crc_repair();
  test_crc_valid_and_invalid();
  test_pairing();
  test_burner();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
