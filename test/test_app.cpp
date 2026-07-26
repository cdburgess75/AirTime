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

// WWV is accepted alongside RDS and contributes fixes.
//
// The station's 220 ms bias is INSIDE the step threshold, so every marker slews
// directly, no corroboration needed — this is the easy half of the WWV story.
// Uncertainty weighting plus the RDS discipline throttle keep the biased
// station from dragging phase back off the minute between windows; the bound
// asserted below is the conservative one (never worse than the RDS bias). The
// hard half — a bias BEYOND the step threshold — is app_wwv_pair_* below.
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

  // WWV fixes are being accepted and credited...
  AT_CHECK(app.displayState().sources & kSrcWwv);
  AT_CHECK(app.displayState().sources & kSrcRds);

  // ...and the clock never settles at the station's bias.
  AT_CHECK(iabs(sim.clockErrorUs(app)) <= 250000);
}

// THE first-day field bug, reproduced end to end. A station biased BEYOND the
// step threshold holds the clock ≥500 ms off true; every WWV marker then
// implies a large correction, which a lone source rightly cannot apply (§4
// rule 3). The old code still told the scheduler "fix!", the window closed,
// and the one piece of evidence that could corroborate — the NEXT minute's
// marker — was discarded. Observed on air: three genuine 800 ms markers,
// three closed windows, sources stuck at "RDS". This test walks the repaired
// chain marker by marker.
AT_TEST(app_lone_large_wwv_marker_keeps_listening) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  sim.wwv.chain_delay_us = 25000;

  // 700 ms late — past the step threshold, well inside RDS's real failure
  // modes (measured on this dial: +0.9 s to +3.2 s CT is common).
  FakeStation s1{9110, 0x1001, true, 700000};
  sim.rds.stations = {s1};

  AppConfig cfg;
  cfg.wwv_calibration_us = 25000;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  // Seed from the liar: the cold seed takes anything, so the trap is armed.
  sim.advance(4 * kMin + 30 * kS, &app);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(iabs(sim.clockErrorUs(app)) > 500000);

  // Open a window mid-minute so the marker arrivals are unambiguous.
  app.operatorListenNow();
  sim.advance(kS, &app);
  AT_CHECK(app.directive().wwv_listening);

  // First marker (~30 s in): implied correction ~700 ms, single source →
  // rejected. The window must survive it, and the clock must not move.
  sim.advance(60 * kS, &app);
  AT_CHECK(app.wwvFixDiag().have);
  AT_CHECK(!app.wwvFixDiag().accepted);
  AT_CHECK(!(app.displayState().sources & kSrcWwv));
  AT_CHECK(app.directive().wwv_listening);
  AT_CHECK(iabs(sim.clockErrorUs(app)) > 500000);

  // Second marker, one minute later, implies the same correction: two
  // independent transmissions agree → support=2 → accepted → NOW the window
  // closes and the clock leaves the bias for the minute edge.
  sim.advance(65 * kS, &app);
  AT_CHECK(app.wwvFixDiag().accepted);
  AT_CHECK(app.wwvFixDiag().corroborated);
  AT_CHECK(app.displayState().sources & kSrcWwv);
  AT_CHECK(!app.directive().wwv_listening);

  // Accepted is not yet applied: §4 rule 1 slews, never steps, and 700 ms at
  // the 500 ppm slew ceiling takes ~23 minutes to inject. (Worth knowing in
  // the field — a big correction does NOT snap in; the display walks to it.)
  sim.advance(30 * kMin, &app);
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 250000);
}

// The same trap left to run for hours on the normal schedule: the hourly
// windows alone must be enough for WWV to get in and stay in.
AT_TEST(app_wwv_pair_corrects_large_rds_bias) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  sim.wwv.chain_delay_us = 25000;

  FakeStation s1{9110, 0x1001, true, 700000};
  sim.rds.stations = {s1};

  AppConfig cfg;
  cfg.wwv_calibration_us = 25000;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(4 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(iabs(sim.clockErrorUs(app)) > 500000);

  sim.advance(3 * kHour, &app);

  AT_CHECK(app.displayState().sources & kSrcWwv);

  // Off the 700 ms trap and onto the minute edge. The bound is tight on
  // purpose: with per-source drift learning the clock converges to a few ms
  // and STAYS there, so a regression in either half of this fix (the pair
  // never forms → ~700 ms; the rate is poisoned by source bias → a sawtooth
  // of hundreds of ms between windows) fails here loudly.
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 100000);

  // And the liar loses its vote: once the clock is disciplined, every
  // correction that station asks for is ≥500 ms, so the arbiter rejects it
  // and RDS quietly stops counting as a live source.
  AT_CHECK_NEAR(app.arbiter().ratePpm(), 18.0, 12.0);
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

// Warm boot after a long power-down — the normal way this device gets used.
//
// THE field failure, end to end: switched off for an hour, NVS hands back an
// hour-old time, and every station on the dial is telling it the truth. It must
// be right within minutes. Observed before the fix: the correction was accepted
// and then slewed at the 500 ppm ceiling, so the device sat 3764 s wrong for the
// entire session while serving NTP at stratum 1 claiming ±110 ms.
AT_TEST(app_warm_boot_stale_by_an_hour_recovers) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -20.0;
  sim.store.has_utc = true;
  sim.store.last_utc_us = startUtcUs() - kHour;  // powered down an hour ago
  sim.store.has_drift = true;
  sim.store.drift_ppm = 20.0;

  FakeStation a{9110, 0x1001, true, 0};
  FakeStation b{9550, 0x1002, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550};
  app.setFmStations(fm, 2);
  app.begin();

  AT_CHECK(iabs(sim.clockErrorUs(app)) > 59 * kMin);   // wakes up an hour out
  AT_CHECK(!app.displayState().synced);                // and admits it

  sim.advance(3 * kMin, &app);

  AT_CHECK(iabs(sim.clockErrorUs(app)) < 300000);      // fixed, within RDS
  AT_CHECK(app.displayState().synced);

  // And the time it hands a laptop is the corrected one, not a promise to be
  // correct in 87 days.
  uint8_t req[kNtpPacketSize], resp[kNtpPacketSize];
  makeNtpRequest(req);
  AT_CHECK(app.handleNtpRequest(req, sizeof(req), 0xC0A80402, resp));
  AT_CHECK_EQ(resp[1], kNtpStratumPrimary);
  AT_CHECK(iabs(ntpToUnixUs(rd64(resp + 40)) - sim.true_utc_us) < 500000);
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

// The device has ONE tuner. Before the arbiter is seeded, WWV listening must
// not run at all: markers are ±30 s ambiguous (pollWwv discards them unseeded),
// and on hardware a pre-seed listen window starves the RDS path that CAN seed.
// First real boot showed the old behaviour: chip parked on AM from t=0.
AT_TEST(app_wwv_defers_until_seeded) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  // No RDS stations: nothing can seed, so listening must never start.
  AirTimeApp app(sim.deps());
  app.begin();

  for (int i = 0; i < 30; ++i) {
    sim.advance(kMin, &app);
    AT_CHECK(!sim.wwv.isRunning());
  }

  // A manual seed makes WWV useful — and allowed.
  app.operatorSetTime(sim.true_utc_us);
  app.operatorListenNow();
  sim.advance(30 * kS, &app);
  AT_CHECK(sim.wwv.isRunning());
}

// While a WWV listen window owns the tuner, the FM dwell rotation must hold
// still. (Ungated, it retuned FM 75 s into every real listen window.)
AT_TEST(app_fm_holds_still_during_wwv_listen) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation a{8990, 0xA920, true, 0};
  FakeStation b{10470, 0x6E47, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {8990, 10470};
  app.setFmStations(fm, 2);
  app.begin();

  sim.advance(4 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  app.operatorListenNow();
  sim.advance(kS, &app);
  AT_CHECK(app.directive().wwv_listening);

  // The window may end EARLY — a marker fix sends the scheduler back to
  // serving — so assert the invariant while it holds: zero retunes for as
  // long as the window owns the tuner (dwell is 75 s, so any rotation would
  // land inside these two minutes).
  const int tunes_at_window_start = sim.rds.tune_count;
  int64_t listened = 0;
  while (app.directive().wwv_listening && listened < 2 * kMin) {
    AT_CHECK_EQ(sim.rds.tune_count, tunes_at_window_start);
    sim.advance(5 * kS, &app);
    listened += 5 * kS;
  }
  // Either we out-waited the dwell inside the window, or the window closed
  // early because a WWV fix landed — both prove the tuner was left alone.
  AT_CHECK(listened >= 80 * kS || (app.displayState().sources & kSrcWwv));

  // Serving again: rotation resumes.
  sim.advance(3 * kMin, &app);
  AT_CHECK(sim.rds.tune_count > tunes_at_window_start);
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
