// End-to-end tests: the real AirTimeApp driven by a simulated ATS Mini.
//
// These are the tests that answer "does the thing actually work?" — a drifting
// crystal, real RDS groups on the air, real WWV markers, a laptop asking for NTP.
// Everything here runs on the host; only the adapters remain to be written.

#include <cstdint>
#include <cstring>

#include "fakes.h"
#include "test_framework.h"

using namespace airtime;
using namespace airtime_fake;

namespace {

constexpr int64_t kS = 1000000;
constexpr int64_t kMin = 60 * kS;
constexpr int64_t kHour = 60 * kMin;

int64_t iabs(int64_t v) { return v < 0 ? -v : v; }

// 2026-07-25T00:00:00Z, on a minute boundary.
int64_t startUtcUs() {
  const int64_t days = civilToMjd(2026, 7, 25) - 40587;
  return days * 86400 * kS;
}

// Build a client NTP request.
void makeNtpRequest(uint8_t* buf) {
  std::memset(buf, 0, kNtpPacketSize);
  buf[0] = (0 << 6) | (4 << 3) | kNtpModeClient;
  buf[2] = 6;
}

uint64_t rd64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

// Cold start with two agreeing RDS stations: the clock seeds, the device starts
// serving, and the time it serves is right.
AT_TEST(app_cold_start_rds_seeds_and_serves) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -20.0;

  FakeStation a{9110, 0x1001, true, 0};
  FakeStation b{9550, 0x1002, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550};
  app.setFmStations(fm, 2);
  app.begin();

  AT_CHECK(!app.arbiter().isSet());
  AT_CHECK(!sim.wifi.isUp());          // §5: WiFi stays down while acquiring

  sim.advance(3 * kMin, &app);

  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(app.scheduler().phase() == Phase::Serving);
  AT_CHECK(sim.wifi.isUp());
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 300000);  // within RDS's own accuracy
}

// No sources at all: we still come up and serve, but we say we are unsynced —
// and NTP marks itself unusable rather than handing out a confident wrong time.
AT_TEST(app_starved_serves_but_flags_unsynced) {
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();

  sim.advance(6 * kMin, &app);

  AT_CHECK(app.scheduler().phase() == Phase::Serving);
  AT_CHECK(!app.arbiter().isSet());

  uint8_t req[kNtpPacketSize], resp[kNtpPacketSize];
  makeNtpRequest(req);
  AT_CHECK(app.handleNtpRequest(req, sizeof(req), 0xC0A80402, resp));
  AT_CHECK_EQ((resp[0] >> 6) & 0x03, kNtpLeapAlarm);
  AT_CHECK_EQ(resp[1], kNtpStratumUnsync);
}

// A station transmitting wrong CT is outvoted by the two that agree.
AT_TEST(app_outvotes_bad_station) {
  Sim sim;
  sim.true_utc_us = startUtcUs();

  FakeStation good1{9110, 0x1001, true, 0};
  FakeStation good2{9550, 0x1002, true, 0};
  FakeStation bad{9930, 0x1003, true, 7 * kS};  // 7 s off
  sim.rds.stations = {good1, good2, bad};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550, 9930};
  app.setFmStations(fm, 3);
  app.begin();

  sim.advance(10 * kMin, &app);

  AT_CHECK(app.arbiter().isSet());
  // The liar is 7 s out; if it had won, the error would be seconds.
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 500000);
}

// WWV pulls the coarse RDS time onto the exact minute boundary.
AT_TEST(app_wwv_refines_phase) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  sim.wwv.chain_delay_us = 25000;  // 25 ms of receive-chain latency

  // One station, deliberately 220 ms late — inside RDS's normal error budget.
  FakeStation s1{9110, 0x1001, true, 220000};
  sim.rds.stations = {s1};

  AppConfig cfg;
  cfg.wwv_calibration_us = 25000;  // calibrated against the chain
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(4 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  // Run through several hourly WWV windows.
  sim.advance(3 * kHour, &app);

  // WWV's phase lock should beat RDS's coarse assertion.
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 100000);
}

// The ADC2/WiFi invariant holds in the real app, not just the scheduler.
AT_TEST(app_never_wifi_and_adc_together) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = 30.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation s1{9110, 0x1001, true, 0};
  sim.rds.stations = {s1};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(5 * kHour, &app);

  AT_CHECK(!sim.adc_wifi_conflict);
  AT_CHECK(sim.wifi.up_count >= 1);
}

// The crystal is learned across hours of operation and persisted.
AT_TEST(app_learns_and_persists_drift) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -28.0;  // device runs slow by 28 ppm

  FakeStation a{9110, 0x1001, true, 0};
  FakeStation b{9550, 0x1002, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550};
  app.setFmStations(fm, 2);
  app.begin();

  sim.advance(8 * kHour, &app);

  // Learned rate should approach +28 ppm to cancel the slow crystal.
  AT_CHECK_NEAR(app.arbiter().ratePpm(), 28.0, 6.0);
  AT_CHECK(sim.store.has_drift);
  AT_CHECK(sim.store.drift_saves >= 1);
}

// Warm boot: stored time comes back, but is honestly reported as unsynced.
AT_TEST(app_warm_boot_restores_unsynced) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.store.has_utc = true;
  sim.store.last_utc_us = startUtcUs();
  sim.store.has_drift = true;
  sim.store.drift_ppm = 12.0;

  AirTimeApp app(sim.deps());
  app.begin();

  AT_CHECK(app.arbiter().isSet());                    // we have a time...
  AT_CHECK(!app.arbiter().isSynced(sim.clock.mono_us)); // ...but do not trust it
  AT_CHECK_NEAR(app.arbiter().ratePpm(), 12.0, 1e-6);  // characterization survived

  const DisplayState st = app.displayState();
  AT_CHECK(st.clock_valid);
  AT_CHECK(!st.synced);
}

// A laptop asking for NTP gets an accurate, stratum-1 answer.
AT_TEST(app_serves_accurate_ntp) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -15.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};

  FakeStation a{9110, 0x1001, true, 0};
  FakeStation b{9550, 0x1002, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550};
  app.setFmStations(fm, 2);
  app.begin();

  sim.advance(2 * kHour, &app);
  AT_CHECK(app.scheduler().phase() == Phase::Serving);

  uint8_t req[kNtpPacketSize], resp[kNtpPacketSize];
  makeNtpRequest(req);
  AT_CHECK(app.handleNtpRequest(req, sizeof(req), 0xC0A80402, resp));

  AT_CHECK_EQ((resp[0] >> 6) & 0x03, kNtpLeapNone);
  AT_CHECK_EQ(resp[1], kNtpStratumPrimary);

  // The served transmit timestamp must be close to true UTC — this is the
  // number that decides whether WSJT-X decodes.
  const int64_t served = ntpToUnixUs(rd64(resp + 40));
  AT_CHECK(iabs(served - sim.true_utc_us) < 500000);

  // A second client is counted distinctly.
  app.handleNtpRequest(req, sizeof(req), 0xC0A80403, resp);
  AT_CHECK_EQ(app.displayState().ntp_clients, 2);
}

// Operator manual set works when nothing is on the air.
AT_TEST(app_manual_set) {
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();
  sim.advance(30 * kS, &app);

  app.operatorSetTime(sim.true_utc_us);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 100000);
  AT_CHECK(app.displayState().sources & kSrcManual);
}
