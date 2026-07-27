// End-to-end tests: the real AirTimeApp driven by a simulated ATS Mini.
//
// These are the tests that answer "does the thing actually work?" — a drifting
// crystal, real RDS groups on the air, real WWV markers, a laptop asking for NTP.
// Everything here runs on the host; only the adapters remain to be written.

#include <cstdint>
#include <cstdio>
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

// WWV teaches RDS, and the point is not the milliseconds — it is that the
// continuous source stops being vetoed.
//
// A station 700 ms late is useless the moment WWV pulls the clock onto the true
// minute: every correction it then asks for exceeds the 500 ms step threshold,
// so §4 rule 3 rejects every one, and the device runs on WWV alone — one fix an
// hour and nothing in between. Measured in simulation before this existed: RDS
// dropped out of the source mask entirely and never came back.
//
// Once its constant lateness is learned and subtracted, the same station is a
// good source again.
AT_TEST(app_learns_station_bias_and_keeps_rds_usable) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  sim.wwv.chain_delay_us = 25000;

  FakeStation s1{9110, 0x1001, true, 700000};   // consistently 700 ms late
  sim.rds.stations = {s1};

  AppConfig cfg;
  cfg.wwv_calibration_us = 25000;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(6 * kHour, &app);

  // The station's error has been measured, and measured as ITS error.
  const StationBias* b = app.stationBias().find(0x1001);
  AT_CHECK(b != nullptr);
  AT_CHECK(b->samples >= 3);
  AT_CHECK_NEAR((double)b->bias_us, 700000.0, 120000.0);

  // ...so it is contributing again rather than being rejected on sight, and
  // the clock is accurate.
  AT_CHECK(app.displayState().sources & kSrcRds);
  AT_CHECK(app.displayState().sources & kSrcWwv);
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 100000);
}

// The correction must never be invented from nothing: with no WWV to teach it,
// a biased station is exactly as biased as before, and the clock says so.
// (Silently "correcting" against an RDS-disciplined clock would measure a
// station against its own error and declare it perfect.)
AT_TEST(app_does_not_invent_bias_without_wwv) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  // No propagating bands: WWV is never heard, so nothing can teach.
  FakeStation s1{9110, 0x1001, true, 220000};
  sim.rds.stations = {s1};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(3 * kHour, &app);

  AT_CHECK_EQ(app.stationBias().correction(0x1001), 0);
  AT_CHECK(!(app.displayState().sources & kSrcWwv));
}

// What the radio learns must survive being switched off — that is the whole
// point of learning it. A station's lateness takes hours of WWV windows to
// establish, and which HF band propagates here is a fact about the location,
// not about this power-on.
AT_TEST(app_remembers_what_it_learned_across_a_power_cycle) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {15000};   // only the high band is open here
  sim.wwv.chain_delay_us = 25000;
  FakeStation s1{9110, 0x1001, true, 700000};
  sim.rds.stations = {s1};

  AppConfig cfg;
  cfg.wwv_calibration_us = 25000;
  {
    AirTimeApp app(sim.deps(), cfg);
    const int32_t fm[] = {9110};
    app.setFmStations(fm, 1);
    app.begin();
    sim.advance(6 * kHour, &app);

    AT_CHECK(app.stationBias().correction(0x1001) != 0);
    AT_CHECK(sim.store.blob_saves > 0);
  }

  // Power cycle: same NVS, brand new everything else.
  AirTimeApp fresh(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  fresh.setFmStations(fm, 1);
  fresh.begin();

  // The station is trusted from the first group, not after another six hours.
  AT_CHECK_NEAR((double)fresh.stationBias().correction(0x1001), 700000.0, 120000.0);

  // And the radio knows where to listen: 15 MHz earned it, so the first window
  // opens there instead of re-hunting 5 MHz.
  AT_CHECK_EQ(fresh.scheduler().bandStats(fresh.scheduler().preferredBandIndex()).khz,
              15000);
}

// A store that has never been written must leave a fresh device exactly as it
// would have been — no phantom corrections, no phantom band preference.
AT_TEST(app_starts_clean_when_nothing_is_stored) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  AirTimeApp app(sim.deps());
  app.begin();
  AT_CHECK_EQ(app.stationBias().count(), 0u);
  AT_CHECK_EQ(app.stationBias().correction(0x1001), 0);
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

  // The stale memory must NOT license WWV listening. On the one-tuner radio
  // that parks the chip on AM, where RDS cannot be read at all, so nothing can
  // end acquisition early and the AP stays down for the full 5-minute timeout
  // — and it invites WWV to phase-lock a clock whose minute is an hour wrong.
  sim.advance(30 * kS, &app);
  AT_CHECK(!sim.wwv.isRunning());

  sim.advance(3 * kMin, &app);

  AT_CHECK(iabs(sim.clockErrorUs(app)) < 300000);      // fixed, within RDS
  AT_CHECK(app.displayState().synced);
  AT_CHECK(sim.wifi.isUp());     // and serving, without waiting out the timeout

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

// Dropped somewhere new, with a station list that means nothing here: the
// radio surveys the dial for itself, adopts what it finds, and remembers it.
//
// This is what separates an instrument from a demo. The compile-time list is
// three frequencies measured on one evening in New Orleans; two states over it
// is worth nothing, RDS never seeds, and the whole clock collapses.
AT_TEST(app_surveys_the_dial_when_it_has_nothing) {
  Sim sim;
  sim.true_utc_us = startUtcUs();

  // A dial the app has never been told about.
  FakeStation a{9370, 0x5001, true, 40000};   a.rssi = 44;
  FakeStation b{10130, 0x5002, true, 90000};  b.rssi = 38;
  FakeStation c{10610, 0x5003, false, 0};     c.rssi = 50;  // loud, but no CT
  sim.rds.stations = {a, b, c};

  AirTimeApp app(sim.deps());
  app.begin();                      // note: NO setFmStations
  AT_CHECK(app.surveying());

  sim.advance(30 * kMin, &app);     // scan pass, then dwell on each candidate

  AT_CHECK(!app.surveying());
  AT_CHECK(sim.store.blobs.count("fm") == 1);   // and it was written down

  // Having found stations, it goes on to do its actual job.
  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());
}

// A radio that already has stations must never be dragged off to go hunting —
// surveying costs the dial for half an hour, and a working clock has something
// to protect.
AT_TEST(app_does_not_survey_when_it_has_stations) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  AT_CHECK(!app.surveying());

  sim.advance(5 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(!app.surveying());
}

// Operator mode: the dial belongs to the human, and the clock keeps time
// anyway. This is what lets the thing be a radio as well as a clock.
AT_TEST(app_radio_mode_stops_tuning_but_keeps_serving) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.crystal_ppm = -18.0;
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation a{9110, 0x1001, true, 0};
  FakeStation b{9550, 0x1002, true, 0};
  sim.rds.stations = {a, b};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110, 9550};
  app.setFmStations(fm, 2);
  app.begin();
  sim.advance(4 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  const int tunes_before = sim.rds.tune_count;
  app.setRadioMode(true);
  sim.advance(2 * kHour, &app);

  // Two hours and it never touched the dial — including no WWV window, which
  // on the one-tuner radio would have yanked the operator off their station.
  AT_CHECK_EQ(sim.rds.tune_count, tunes_before);
  AT_CHECK(!sim.wwv.isRunning());

  // ...and WiFi never dropped, so NTP served the whole time. In clock mode an
  // hourly listen window takes it down; here there is nothing to schedule
  // around, so service is better, not worse.
  AT_CHECK(sim.wifi.isUp());
  uint8_t req[kNtpPacketSize], resp[kNtpPacketSize];
  makeNtpRequest(req);
  AT_CHECK(app.handleNtpRequest(req, sizeof(req), 0xC0A80402, resp));
  AT_CHECK_EQ(resp[1], kNtpStratumPrimary);

  // Coasting is cheap once the crystal is known: two hours costs milliseconds,
  // not seconds. This is the number that makes operator mode affordable.
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 200000);

  // Back to clock mode and it resumes disciplining itself.
  app.setRadioMode(false);
  sim.advance(10 * kMin, &app);
  AT_CHECK(sim.rds.tune_count > tunes_before);
}

// Leaving operator mode has to assume the worst about the dial.
//
// While the mode is on, the human tunes the chip by hand and AirTime does not
// watch. Both retune paths short-circuit on a cached value — the WWV path skips
// tuneKhz() when the wanted band already matches, the FM path waits for a dwell
// timer — so both would happily go on describing a frequency the radio left
// long ago. The operator parks on 40 m; the log keeps saying 15000.
//
// This is the single-tuner class of bug (PLAN.md §2) in its quietest form: no
// conflict, no error, just a receiver pointed somewhere else than the software
// believes, which shows up only as a listen window that hears nothing.
AT_TEST(app_radio_mode_exit_retunes_from_wherever_the_operator_left_it) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  // One propagating band and one station, so neither recovery path can happen
  // by accident: the scheduler will want the SAME band next window (which is
  // what makes the equality test skip the retune), and with a single station
  // there is no dwell rotation to re-tune the FM side behind our back.
  sim.wwv.propagating_bands = {15000};
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  // Four hours, because the trap needs the scheduler to have SETTLED. While it
  // is still hunting, every window opens on a different band and retunes on the
  // way in, which hides the bug. Once 15 MHz has proved itself the sampler stops
  // being retuned at all -- the wanted band equals the believed band, window
  // after window -- and that is precisely when a stale belief becomes permanent.
  sim.advance(4 * kHour, &app);
  AT_CHECK(app.arbiter().isSet());
  const int32_t band_before = sim.wwv.tunedKhz();
  AT_CHECK_EQ(band_before, 15000);

  const int settled_tunes = sim.wwv.tune_count;
  sim.advance(1 * kHour, &app);
  AT_CHECK_EQ(sim.wwv.tune_count, settled_tunes);   // settled: no retunes at all

  // The operator takes the dial and parks it on 40 m, which is what the
  // firmware's selectBand() does to the chip underneath both adapters.
  app.setRadioMode(true);
  sim.advance(20 * kMin, &app);
  sim.rds.tuneKhz(7200);
  sim.wwv.tuneKhz(7200);
  const int rds_tunes = sim.rds.tune_count;
  const int wwv_tunes = sim.wwv.tune_count;

  // Handing the dial back must put the FM station back immediately -- not at
  // the next dwell rotation, which with one station never comes.
  app.setRadioMode(false);
  app.loop();
  AT_CHECK_EQ(sim.rds.tune_count, rds_tunes + 1);
  AT_CHECK_EQ(sim.rds.tunedKhz(), 9110);

  // ...and the next listen window must retune WWV even though the band it
  // wants is the very one it last asked for. That equality is exactly what made
  // this invisible: same band, so "already there", so no retune -- and the
  // window opens on 7200 kHz and hears nothing, forever.
  sim.advance(90 * kMin, &app);
  AT_CHECK(sim.wwv.tune_count > wwv_tunes);
  AT_CHECK_EQ(sim.wwv.tunedKhz(), band_before);
}

// The §2 invariant must hold in operator mode too — it is the one rule that
// can damage a measurement rather than merely annoy the operator.
AT_TEST(app_radio_mode_never_breaks_the_adc_rule) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  for (int i = 0; i < 12; ++i) {           // toggle across many listen windows
    app.setRadioMode(i % 2 == 0);
    sim.advance(25 * kMin, &app);
  }
  AT_CHECK(!sim.adc_wifi_conflict);
}

// ── CW decode ───────────────────────────────────────────────────────────────
//
// End to end: a real keying pattern, through the sampler seam, through the app,
// onto the buffer a screen reads. The decoder itself is covered in test_morse;
// what this pins down is the WIRING — that the app puts the sampler in the
// right state, that the blocks reach the decoder rather than the marker
// detector, and that the §2 rule holds in a mode where the audio tap is live
// for as long as the operator wants it.
namespace {

// Key a message into a fake sampler's queue, exactly as the ESP32 task would:
// 5 ms blocks of tone power at the levels Milestone 0 measured on the tap.
class CwKeyer {
 public:
  CwKeyer(FakeWwvSampler* s, int64_t start_us, int wpm)
      : s_(s), now_(start_us), unit_(1200000 / wpm) {}

  void mark(int64_t us) { blocks(us, 1.9e-3f); }
  void space(int64_t us) { blocks(us, 7.7e-5f); }

  // "CQ DE W1AW" style, using the standard element notation per letter.
  void letter(const char* elements) {
    for (const char* p = elements; *p; ++p) {
      mark(*p == '-' ? unit_ * 3 : unit_);
      if (p[1]) space(unit_);
    }
  }
  void letterGap() { space(unit_ * 3); }
  void wordGap() { space(unit_ * 7); }
  int64_t now() const { return now_; }

 private:
  void blocks(int64_t dur_us, real power) {
    for (int64_t t = 0; t < dur_us; t += 5000) {
      s_->pushPower(now_, power);
      now_ += 5000;
    }
  }
  FakeWwvSampler* s_;
  int64_t now_;
  int64_t unit_;
};

}  // namespace

AT_TEST(app_cw_mode_decodes_traffic_onto_the_screen) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};
  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  app.setCwMode(true);
  app.loop();
  AT_CHECK(app.cwMode());
  AT_CHECK(sim.wwv.isRunning());     // the tap is live...
  AT_CHECK(!sim.wifi.isUp());        // ...so the radio is off. Non-negotiable.

  // "CQ DE W1AW" at 18 WPM, the speed W1AW sends bulletins at.
  CwKeyer k(&sim.wwv, sim.clock.mono_us, 18);
  struct { const char* ch; } msg[] = {
      {"-.-."}, {"--.-"}, {nullptr},          // CQ
      {"-.."},  {"."},    {nullptr},          // DE
      {".--"},  {".----"},{".-"}, {".--"},    // W1AW
  };
  for (std::size_t i = 0; i < sizeof(msg) / sizeof(msg[0]); ++i) {
    if (msg[i].ch == nullptr) { k.wordGap(); continue; }
    k.letter(msg[i].ch);
    if (i + 1 < sizeof(msg) / sizeof(msg[0]) && msg[i + 1].ch != nullptr)
      k.letterGap();
  }
  // The last letter needs more than a letter gap to come out. A character is
  // only known to be finished when the NEXT one starts, or when the sender
  // clearly stops (MorseConfig::idle_flush_us, 3 s) -- so live copy always
  // trails the air by one character, and 600 ms of silence would leave the
  // final W of W1AW still buffered. That is the decoder being right, not slow.
  k.space(3500000);
  app.loop();

  AT_CHECK_EQ(std::strcmp(app.cwText().text(), "CQ DE W1AW"), 0);
  AT_CHECK(app.cwStatus().wpm >= 15 && app.cwStatus().wpm <= 21);

  // Leaving hands everything back: the dial is re-tuned, the tap released, and
  // the access point comes up so the device is a clock again.
  const int tunes = sim.rds.tune_count;
  app.setCwMode(false);
  app.loop();
  AT_CHECK(!app.cwMode());
  AT_CHECK(!sim.wwv.isRunning());
  AT_CHECK_EQ(sim.rds.tune_count, tunes + 1);
  sim.advance(2 * kMin, &app);
  AT_CHECK(sim.wifi.isUp());
}

// The §2 invariant is what makes this mode dangerous to get wrong: it holds the
// audio tap open indefinitely, at the operator's pleasure, rather than for a
// scheduled three minutes. Sim::advance asserts wifi-and-ADC are never both
// live, so an hour of toggling is a real test of it.
AT_TEST(app_cw_mode_never_breaks_the_adc_rule) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};
  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();

  for (int i = 0; i < 8; ++i) {
    app.setCwMode(i % 2 == 0);
    sim.advance(20 * kMin, &app);
  }
  app.setCwMode(false);
  AT_CHECK(!sim.adc_wifi_conflict);
}

// CW blocks must never reach the marker detector. They are 5 ms of a beat note
// keyed by a human; the detector gates 700-900 ms bursts and would happily
// classify a dah at 12 WPM as a minute marker, handing the arbiter a phase
// measurement derived from somebody's callsign.
AT_TEST(app_cw_traffic_never_disciplines_the_clock) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};
  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(10 * kMin, &app);

  const uint32_t markers_before = app.wwvMarker().diag().markers;
  const int64_t err_before = sim.clockErrorUs(app);

  app.setCwMode(true);
  app.loop();
  // 5 WPM: a dah is 720 ms, squarely inside the 700-900 ms marker gate.
  CwKeyer k(&sim.wwv, sim.clock.mono_us, 5);
  for (int i = 0; i < 6; ++i) { k.letter("-"); k.letterGap(); }
  k.space(600000);
  app.loop();

  AT_CHECK_EQ(app.wwvMarker().diag().markers, markers_before);
  AT_CHECK(iabs(sim.clockErrorUs(app) - err_before) < 1000);
  AT_CHECK(app.cwText().size() > 0);   // ...but it WAS decoded
}
