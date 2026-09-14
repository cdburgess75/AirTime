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
// With nothing proven on FM the radio listens for HF first (an 8-minute window
// from about 2.5 minutes in), so serving is checked once that window is over.
AT_TEST(app_starved_serves_but_flags_unsynced) {
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();

  sim.advance(12 * kMin, &app);

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
  // This test opens its own window at a chosen moment; the automatic one after
  // an unconfirmed seed is scheduling policy, tested on its own elsewhere.
  cfg.unconfirmed_listen_after_us = 0;
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
  // The framework reports and continues, so a null here must end the test —
  // dereferencing it takes down the whole binary before the summary prints,
  // and a crash-in-place is the one way this suite can fail SILENTLY: a runner
  // that greps for FAIL lines sees none and reads the wreck as a pass. That
  // exact misreading cost a debugging session during the tuner redesign.
  if (b == nullptr) return;
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

// The device has ONE tuner, and the boot hunt belongs to RDS: through the
// whole ACQUIRING phase, WWV listening must not run — a pre-seed window there
// starves the one path that seeds in about a minute when it works at all.
// (First real boot showed the old failure: chip parked on AM from t=0.)
// After acquisition times out, unseeded listen windows are allowed — that is
// the timecode's job now — so this test pins the boundary, not a blanket ban.
AT_TEST(app_wwv_defers_to_rds_while_acquiring) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  sim.wwv.sub_carrier_present = false;  // nothing for a window to find
  // No RDS stations either: acquisition runs its full course.
  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  app.begin();

  while (app.scheduler().phase() == Phase::Acquiring) {
    AT_CHECK(!sim.wwv.isRunning());
    sim.advance(10 * kS, &app);
  }
  AT_CHECK(app.scheduler().phase() == Phase::Serving);

  // A manual seed still opens the gate on demand, exactly as before.
  app.operatorSetTime(sim.true_utc_us);
  app.operatorListenNow();
  sim.advance(30 * kS, &app);
  AT_CHECK(sim.wwv.isRunning());
}

// ── The structural fix for the FM-poor QTH ──────────────────────────────────
// No FM stations at all, a band that propagates, and nobody touching the
// radio: the 100 Hz timecode must start the clock BY ITSELF. Acquiring times
// out (RDS had its chance), the unseeded cadence opens a listen window, the
// decoder hunts the frame, reads two whole frames in lockstep, and the
// arbiter seeds from a fix that knows the date. This is the test that says
// the hurricane case works.
AT_TEST(app_timecode_cold_starts_from_hf_alone) {
  Sim sim;
  sim.true_utc_us = startUtcUs() + 17 * kMin + 23 * kS;  // nothing aligned
  sim.crystal_ppm = 12.0;
  sim.wwv.propagating_bands = {10000};

  AppConfig cfg;
  cfg.auto_survey = false;   // keep the dial quiet; nothing to survey anyway
  AirTimeApp app(sim.deps(), cfg);
  app.begin();

  // Acquire (5 min) + up to one unseeded interval (15 min) + a window long
  // enough to sweep dead bands onto 10 MHz and read the code. Step small
  // enough that pump() emits every 50 ms sub block.
  bool seeded = false;
  for (int i = 0; i < 40 * 60 && !seeded; ++i) {
    sim.advance(kS, &app, 10000);
    seeded = app.arbiter().hasSourceFix();
  }

  AT_CHECK(seeded);
  AT_CHECK(app.arbiter().isSet());
  const int64_t now = sim.clock.mono_us;
  AT_CHECK(app.arbiter().isSynced(now));
  // Identity is the claim: the right MINUTE, and phase inside the timecode's
  // own ±250 ms model (block quantisation + the 30 ms lead, both simulated).
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 300000);
  // The screen should say WWV did this.
  AT_CHECK((app.displayState().sources & kSrcWwv) != 0);
  AT_CHECK(app.wwvTimecodeDiag().have);
  AT_CHECK(app.wwvTimecodeDiag().accepted);
  AT_CHECK(app.wwvTimecode().framesConfirmed() >= 1);
  // The silicon rule survived the whole unseeded dance.
  AT_CHECK(!sim.adc_wifi_conflict);
  // And the fix ended the window: the device went back to serving.
  sim.advance(5 * kS, &app);
  AT_CHECK(sim.wifi.isUp());
}

// The same cold start at the levels the owner's radio reported in the field,
// not the bench: marker bin noise ~1.5e-5 and peak ~1.5e-4 (mkr[]), subcarrier
// peak ~2.9e-5 (code[]), about 5x under Milestone 0. The bench thresholds sat
// above every one of those peaks, so the radio listened for hours and never
// counted a single pulse while WWV was audible in the speaker.
AT_TEST(app_timecode_cold_starts_at_field_levels) {
  Sim sim;
  sim.true_utc_us = startUtcUs() + 17 * kMin + 23 * kS;
  sim.crystal_ppm = 12.0;
  sim.wwv.propagating_bands = {10000};
  sim.wwv.tone_power = 1.5e-4f;
  sim.wwv.noise_power = 1.5e-5f;
  sim.wwv.sub_tone_power = 2.9e-5f;
  sim.wwv.sub_noise_power = 6.0e-6f;

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  app.begin();

  bool seeded = false;
  for (int i = 0; i < 40 * 60 && !seeded; ++i) {
    sim.advance(kS, &app, 10000);
    seeded = app.arbiter().hasSourceFix();
  }

  AT_CHECK(app.wwvTimecodeDiag().pulses > 0);
  AT_CHECK(seeded);
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 300000);
  AT_CHECK((app.displayState().sources & kSrcWwv) != 0);
}

// Same sky, but the subcarrier is absent (band propagates, code unreadable):
// the device must keep LOOKING without ever inventing a time. The marker path
// alone stays gated unseeded — a boundary with no identity — so hours later
// the clock is still honestly unset and NTP still flags itself unusable.
AT_TEST(app_timecode_absent_never_fakes_a_seed) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {10000};
  sim.wwv.sub_carrier_present = false;

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  app.begin();

  sim.advance(2 * kHour, &app, 10000);

  AT_CHECK(!app.arbiter().hasSourceFix());
  AT_CHECK(!app.displayState().clock_valid);
  AT_CHECK(!sim.adc_wifi_conflict);
  // Markers were heard (the band table may learn) — but never believed.
  AT_CHECK(app.wwvMarker().diag().markers > 0);
  AT_CHECK(!app.wwvFixDiag().have);
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
  app.setMode(OpMode::Radio);
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
  app.setMode(OpMode::Clock);
  sim.advance(10 * kMin, &app);
  AT_CHECK(sim.rds.tune_count > tunes_before);
}

// One tuner, one truth: a listen window must find the chip actually ON the
// band it believes it chose, and must hand the chip back to FM when it closes.
//
// Both halves of this were real bugs, and both were invisible while the fakes
// were two independent radios:
//
//  * With one FM station, nothing retuned after a window — the dwell rotation
//    requires station_count_ > 1 — so the chip sat on AM forever and RDS went
//    permanently deaf while the station list looked healthy.
//  * With the window-end retune in place, `tuned_wwv_khz_` went stale instead:
//    the next window wanted the same band the cache already claimed, skipped
//    the retune, and spent three minutes sampling FM program audio. On the
//    device that read as "music on 10 MHz" and a jamming theory.
AT_TEST(app_listen_windows_own_the_real_tuner_and_give_it_back) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  // One propagating band and ONE station: no dwell rotation exists to paper
  // over a missing retune, and the settled scheduler wants the same band every
  // window — the exact condition under which the stale cache was permanent.
  sim.wwv.propagating_bands = {15000};
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  AT_CHECK(sim.tuner.onFm(9110));   // acquiring = harvesting RDS

  // Let it find 15 MHz and settle into the hourly rhythm.
  sim.advance(4 * kHour, &app);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK_EQ(sim.wwv.tunedKhz(), 15000);

  // Settled means: between windows the chip is on FM (or RDS starves), and
  // each window REALLY retunes — the chip, not just the cache — despite asking
  // for the very band it asked for last time.
  AT_CHECK(sim.tuner.onFm(9110));
  app.operatorListenNow();
  sim.advance(30 * kS, &app);
  AT_CHECK(app.directive().wwv_listening);
  AT_CHECK(sim.tuner.onAm(15000));           // truth, not the cache
  sim.advance(5 * kMin, &app);               // window closes (exit on fix)
  AT_CHECK(!app.directive().wwv_listening);
  AT_CHECK(sim.tuner.onFm(9110));            // ...and the dial came back

  // RDS is therefore still alive hours later — the single-station deafness.
  sim.advance(2 * kHour, &app);
  AT_CHECK(app.displayState().sources & kSrcRds);

  // The operator takes the dial and parks it on 40 m. selectBand() touches the
  // chip through neither adapter, so both caches go stale — as on hardware.
  app.setMode(OpMode::Radio);
  sim.advance(20 * kMin, &app);
  sim.operatorTune(7200, /*fm=*/false);
  AT_CHECK_EQ(sim.rds.tunedKhz(), 9110);     // the cache's stale claim...
  AT_CHECK(sim.tuner.onAm(7200));            // ...vs where the chip really is

  // Handing the dial back re-tunes the chip immediately — not at the next
  // dwell rotation, which with one station never comes.
  app.setMode(OpMode::Clock);
  AT_CHECK(sim.tuner.onFm(9110));

  // And the next window still finds its band for real.
  app.operatorListenNow();
  sim.advance(30 * kS, &app);
  AT_CHECK(sim.tuner.onAm(15000));
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
    app.setMode(i % 2 == 0 ? OpMode::Radio : OpMode::Clock);
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
  // Mode mechanics, not scheduling: no automatic window after the Yellow seed.
  AppConfig cfg;
  cfg.unconfirmed_listen_after_us = 0;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  app.setMode(OpMode::Cw);
  app.loop();
  AT_CHECK(app.cwMode());
  AT_CHECK(sim.wwv.isRunning());     // the tap is live...
  AT_CHECK(!sim.wifi.isUp());        // ...so the radio is off. Non-negotiable.

  // The shared detector was genuinely repointed — the Goertzel bin at the CW
  // note, blocks short enough to resolve a 40 WPM dit — and the repointing
  // never raced a running sampler.
  AT_CHECK_EQ((double)sim.wwv.detector_tone_hz, 700.0);
  AT_CHECK_EQ(sim.wwv.detector_block_us, 5000);
  AT_CHECK_EQ(sim.wwv.rejected_detector_sets, 0);

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

  // Leaving hands everything back: the dial re-tuned exactly once, the tap
  // released, the detector back on WWV's numbers, and the access point up so
  // the device is a clock again.
  const int tunes = sim.rds.tune_count;
  app.setMode(OpMode::Clock);
  app.loop();
  AT_CHECK(!app.cwMode());
  AT_CHECK(!sim.wwv.isRunning());
  AT_CHECK_EQ(sim.rds.tune_count, tunes + 1);
  AT_CHECK(sim.tuner.onFm(9110));
  AT_CHECK_EQ((double)sim.wwv.detector_tone_hz, 1000.0);
  AT_CHECK_EQ(sim.wwv.detector_block_us, 20000);
  AT_CHECK_EQ(sim.wwv.rejected_detector_sets, 0);
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

  static const OpMode kCycle[] = {OpMode::Cw, OpMode::Clock, OpMode::Spectrum,
                                  OpMode::Radio, OpMode::Cw, OpMode::Spectrum,
                                  OpMode::Clock, OpMode::Radio};
  for (int i = 0; i < 8; ++i) {
    app.setMode(kCycle[i]);
    sim.advance(20 * kMin, &app);
  }
  app.setMode(OpMode::Clock);
  AT_CHECK(!sim.adc_wifi_conflict);
}

// The waterfall owns the tap exactly as CW does and pays the same rent: WiFi
// down while it is on screen, dial untouched underneath the operator, and a
// clean handback — sampler released, FM re-tuned, AP restored — on exit.
AT_TEST(app_spectrum_mode_owns_the_tap_and_hands_it_back) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};
  // Mode mechanics, not scheduling: no automatic window after the Yellow seed.
  AppConfig cfg;
  cfg.unconfirmed_listen_after_us = 0;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  app.setMode(OpMode::Spectrum);
  app.loop();
  AT_CHECK(sim.wwv.isRunning());     // the tap is live...
  AT_CHECK(!sim.wifi.isUp());        // ...so the radio is off. Same rule as CW.

  // Half an hour of watching: AirTime must not retune underneath the operator,
  // and the §2 invariant must hold throughout.
  const int tunes = sim.rds.tune_count;
  sim.advance(30 * kMin, &app);
  AT_CHECK_EQ(sim.rds.tune_count, tunes);
  AT_CHECK(!sim.adc_wifi_conflict);

  app.setMode(OpMode::Clock);
  AT_CHECK(!sim.wwv.isRunning());    // released synchronously on exit
  AT_CHECK(sim.tuner.onFm(9110));
  // One station's time is still unconfirmed, so a WWV window is due at once
  // and may take the AP for its eight minutes. After that, it is back.
  sim.advance(12 * kMin, &app);
  AT_CHECK(sim.wifi.isUp());
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

  app.setMode(OpMode::Cw);
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

AT_TEST(app_manual_time_breaks_the_cold_start_deadlock) {
  // The deadlock: WWV's minute marker carries phase but not identity, so with
  // no RDS the device can never start a clock at all. effectiveDirective()
  // refuses to open a listen window until the arbiter has a SOURCE fix,
  // precisely because a marker alone cannot resolve which minute it is.
  //
  // An operator with a wristwatch supplies the missing identity. That is all
  // WWV needs — its window is +/-30 s.
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();
  AT_CHECK(!app.arbiter().hasSourceFix());   // shut: no listening possible

  app.setManualUtc(sim.true_utc_us);
  AT_CHECK(app.arbiter().hasSourceFix());    // open: WWV may now listen
  AT_CHECK(app.displayState().clock_valid);
}

AT_TEST(app_manual_time_does_not_claim_to_be_synchronised) {
  // A wristwatch is not a time standard and the device must not say it is.
  // At +/-5 s the estimate is an order of magnitude outside the 1 s sync
  // threshold, so the clock stays flagged UNSYNCED and NTP keeps telling
  // clients not to trust it — while still being good enough to bootstrap WWV,
  // which is the entire job. Serving authoritative time off a manual entry is
  // exactly the "confidently wrong" failure this project keeps meeting.
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();
  app.setManualUtc(sim.true_utc_us);

  const auto st = app.displayState();
  AT_CHECK(st.clock_valid);
  AT_CHECK(!st.synced);
  AT_CHECK(st.uncertainty_us >= 1000000);
  AT_CHECK(st.uncertainty_us <= 10000000);
  // ...and it landed where it was told, within the sim's own advance.
  AT_CHECK(st.utc_us > sim.true_utc_us - 2000000);
  AT_CHECK(st.utc_us < sim.true_utc_us + 2000000);
}

AT_TEST(app_manual_time_is_inside_the_wwv_ambiguity_window) {
  // The number that matters: WWV can only resolve the minute if the clock is
  // already within half a minute. A manual set claiming 5 s clears that with
  // an order of magnitude to spare, which is why this is a real escape hatch
  // and not a gesture.
  Sim sim;
  sim.true_utc_us = startUtcUs();

  AirTimeApp app(sim.deps());
  app.begin();
  app.setManualUtc(sim.true_utc_us + 4000000);   // operator four seconds late

  const int64_t err = app.displayState().utc_us - sim.true_utc_us;
  AT_CHECK(err < 30000000 && err > -30000000);
}

// The firmware hands the app its compiled seed BEFORE begin(). A list the
// radio measured for itself must still win, or every survey is thrown away at
// the next power-on — which is how the owner's radio stayed on a list that
// never delivered a single RDS group.
AT_TEST(app_saved_stations_win_over_the_compiled_seed) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation seed{9110, 0x1001, true, 0};
  FakeStation measured{10470, 0x6E47, true, 0};
  sim.rds.stations = {seed, measured};

  const int32_t saved[] = {10470};
  uint8_t blob[kStationsBlobMax];
  const std::size_t n = encodeStations(saved, 1, blob, sizeof(blob));
  sim.store.blobs["fm"].assign(blob, blob + n);

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);   // the firmware's order: seed first...
  app.begin();                // ...then begin()

  AT_CHECK_EQ(sim.rds.tunedKhz(), 10470);
  AT_CHECK(app.stationCount() == 1);
  AT_CHECK_EQ(app.station(0), 10470);
}

// The compiled seed is a guess. Saving it back as though it had been measured
// is how a researched-but-wrong list became permanent on the device.
AT_TEST(app_seed_is_never_persisted_as_measured) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{9110, 0x1001, true, 0};
  sim.rds.stations = {a};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {9110};
  app.setFmStations(fm, 1);
  app.begin();
  app.setManualUtc(sim.true_utc_us);   // learned state now has something to save
  sim.advance(2 * kHour, &app);        // past the save interval

  AT_CHECK(sim.store.blob_saves > 0);            // learned state WAS written...
  AT_CHECK(sim.store.blobs.count("fm") == 0);    // ...but never the seed
}

// A list is a guess until it delivers. When no station on it sends clock time,
// the radio goes and finds stations that do — the owner's radio sat on a silent
// list for a day and a half with plenty of clock-time stations on the dial.
AT_TEST(app_surveys_when_its_list_stays_silent) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation dead{8930, 0x1001, false, 0};   dead.rssi = 30;   // listed, no CT
  FakeStation live{10470, 0x6E47, true, 0};   live.rssi = 45;   // not listed
  sim.rds.stations = {dead, live};

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {8930};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(9 * kMin, &app);
  AT_CHECK(!app.surveying());        // the list gets a fair run first
  sim.advance(2 * kMin, &app);
  AT_CHECK(app.surveying());

  sim.advance(20 * kMin, &app);      // survey finishes and adopts what it heard
  AT_CHECK(!app.surveying());
  AT_CHECK_EQ(app.station(0), 10470);
  AT_CHECK(sim.store.blobs.count("fm") == 1);

  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());
}

// A survey owns the dial for a quarter of an hour. A dial with nothing on it
// must not be re-hunted every ten minutes.
AT_TEST(app_silent_list_survey_is_rate_limited) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation dead{8930, 0x1001, false, 0};   dead.rssi = 30;
  sim.rds.stations = {dead};          // nothing on the whole dial sends CT

  AirTimeApp app(sim.deps());
  const int32_t fm[] = {8930};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(40 * kMin, &app);       // one survey ran and found nothing
  AT_CHECK(!app.surveying());
  sim.advance(2 * kHour, &app);
  AT_CHECK(!app.surveying());         // and it did not go hunting again
}

// One honest station and one that sends a wrong time, a single report each,
// with the liar reporting FIRST. The clock is already right. The liar must not
// win the 1-1 vote by list order: the arbiter would refuse it every time and
// the honest station would never steer again — exactly what the owner's radio
// did for half an hour with a CT date 12.6 days wrong.
AT_TEST(app_honest_station_wins_a_tie_against_a_liar) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation liar{9110, 0x1001, true, 20 * kMin};
  FakeStation honest{10470, 0x6E47, true, 0};
  sim.rds.stations = {liar, honest};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9110, 10470};    // the liar is tuned, and heard, first
  app.setFmStations(fm, 2);
  app.begin();
  app.setManualUtc(sim.true_utc_us);     // the clock we already have is right

  sim.advance(12 * kMin, &app);          // both have reported; votes keep coming
  AT_CHECK(app.rdsFixDiag().have);
  AT_CHECK(app.rdsFixDiag().accepted);   // the latest vote went to the honest one
  const int64_t err = app.displayState().utc_us - sim.true_utc_us;
  AT_CHECK(err < 1000000 && err > -1000000);
}

// The owner's rule, end to end: the first station sets the clock and is only
// Yellow; when a DIFFERENT station agrees, both are Green.
AT_TEST(app_one_station_is_yellow_until_another_agrees) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{10470, 0x6E47, true, 0};
  FakeStation b{8990, 0xA920, true, 0};
  sim.rds.stations = {a, b};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10470, 8990};
  app.setFmStations(fm, 2);
  app.begin();

  sim.advance(70 * kS, &app);                    // first dwell: only 104.7
  const SourceRow* ra = app.sources().find(SourceKind::Fm, 10470);
  AT_CHECK(ra != nullptr);
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(app.sources().rating(*ra) == Rating::Yellow);

  sim.advance(4 * kMin, &app);                   // 89.9 is heard and agrees
  const SourceRow* rb = app.sources().find(SourceKind::Fm, 8990);
  AT_CHECK(rb != nullptr);
  AT_CHECK(app.sources().rating(*rb) == Rating::Green);
  AT_CHECK(app.sources().rating(*ra) == Rating::Green);
}

// A station whose time is wrong against a confirmed clock turns Red and stops
// voting; the clock stays right.
AT_TEST(app_wrong_station_turns_red) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{10470, 0x6E47, true, 0};
  FakeStation b{8990, 0xA920, true, 0};
  FakeStation liar{9110, 0x1001, true, 20 * kMin};
  sim.rds.stations = {a, b, liar};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10470, 8990, 9110};
  app.setFmStations(fm, 3);
  app.begin();
  sim.advance(20 * kMin, &app);

  const SourceRow* rl = app.sources().find(SourceKind::Fm, 9110);
  AT_CHECK(rl != nullptr);
  AT_CHECK(app.sources().rating(*rl) == Rating::Red);
  const int64_t err = app.displayState().utc_us - sim.true_utc_us;
  AT_CHECK(err < 1000000 && err > -1000000);
}

// A station that never sends clock time turns Red after two whole dwells and
// is then passed over, instead of costing a dwell every pass.
AT_TEST(app_silent_station_turns_red_and_is_passed_over) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation mute{8930, 0x1001, false, 0};
  FakeStation a{10470, 0x6E47, true, 0};
  sim.rds.stations = {mute, a};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {8930, 10470};
  app.setFmStations(fm, 2);
  app.begin();
  sim.advance(15 * kMin, &app);

  const SourceRow* rm = app.sources().find(SourceKind::Fm, 8930);
  AT_CHECK(rm != nullptr);
  AT_CHECK(app.sources().rating(*rm) == Rating::Red);

  int on_mute = 0;
  for (int i = 0; i < 60; ++i) {                 // the next half hour
    sim.advance(30 * kS, &app);
    if (sim.rds.tunedKhz() == 8930) ++on_mute;
  }
  AT_CHECK_EQ(on_mute, 0);
}

// Ratings are remembered, and the next power-on tries Green stations first and
// Red ones last.
AT_TEST(app_ratings_survive_a_power_cycle) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation mute{8930, 0x1001, false, 0};
  FakeStation a{10470, 0x6E47, true, 0};
  FakeStation b{8990, 0xA920, true, 0};
  sim.rds.stations = {mute, a, b};

  AppConfig cfg;
  cfg.auto_survey = false;
  const int32_t fm[] = {8930, 10470, 8990};
  {
    AirTimeApp app(sim.deps(), cfg);
    app.setFmStations(fm, 3);
    app.begin();
    sim.advance(2 * kHour, &app);                // past the save interval
    AT_CHECK(sim.store.blobs.count("src") == 1);
  }

  AirTimeApp again(sim.deps(), cfg);             // same store: a power cycle
  again.setFmStations(fm, 3);
  again.begin();
  const SourceRow* ra = again.sources().find(SourceKind::Fm, 10470);
  AT_CHECK(ra != nullptr);
  AT_CHECK(again.sources().rating(*ra) == Rating::Green);
  AT_CHECK(again.station(0) != 8930);            // a Green station leads
  AT_CHECK_EQ(again.station(2), 8930);           // the Red one comes last
}

// The group-type count is how the log tells "no clock time on this dial" from
// "clock time lost on the way in": a CT station must show 4A, and only 4A.
AT_TEST(app_counts_rds_group_types_per_station) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{10470, 0x6E47, true, 0};
  sim.rds.stations = {a};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10470};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(3 * kMin, &app);

  AT_CHECK(app.rdsGroupsOfType(4, false) > 0);
  AT_CHECK(app.rdsGroupsOfType(0, false) == 0);
  AT_CHECK(app.rdsGroupsOfType(4, true) == 0);
}

// The clock itself carries the rule: set from one station it is unconfirmed,
// and only a different source agreeing makes it confirmed.
AT_TEST(app_clock_is_unconfirmed_until_a_second_source_agrees) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation a{10470, 0x6E47, true, 0};
  FakeStation b{8990, 0xA920, true, 0};
  sim.rds.stations = {a, b};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10470, 8990};
  app.setFmStations(fm, 2);
  app.begin();

  sim.advance(70 * kS, &app);
  AT_CHECK(app.displayState().synced);
  AT_CHECK(!app.displayState().confirmed);
  sim.advance(4 * kMin, &app);
  AT_CHECK(app.displayState().confirmed);
}

// 92.3 on the owner's dial: the only clock-time station, five minutes slow.
// Set by hand, the clock is good to seconds — enough to call that station Red
// and keep it out of the vote, though not enough to confirm anything.
AT_TEST(app_hand_set_clock_turns_a_minutes_wrong_station_red) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation slow{9230, 0x986D, true, 5 * kMin};
  sim.rds.stations = {slow};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9230};
  app.setFmStations(fm, 1);
  app.begin();
  app.setManualUtc(sim.true_utc_us);
  // A seeded clock may listen for WWV straight through the 5-minute acquisition
  // window, and the tuner is single: the station is heard once serving begins.
  sim.advance(15 * kMin, &app);

  const SourceRow* r = app.sources().find(SourceKind::Fm, 9230);
  AT_CHECK(r != nullptr);
  AT_CHECK(app.sources().rating(*r) == Rating::Red);
  const int64_t err = app.displayState().utc_us - sim.true_utc_us;
  AT_CHECK(err < 10 * kS && err > -10 * kS);
  AT_CHECK(!app.displayState().confirmed);
}

// The order it really happened in on the owner's radio: 92.3 set the clock
// first, five minutes slow, and the owner set it by hand afterwards.
AT_TEST(app_hand_set_after_a_slow_station_synced_the_clock) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation slow{9230, 0x986D, true, 5 * kMin};
  sim.rds.stations = {slow};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9230};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(20 * kMin, &app);
  AT_CHECK(app.displayState().utc_us - sim.true_utc_us < -4 * kMin);   // on 92.3's time

  app.setManualUtc(sim.true_utc_us);
  sim.advance(15 * kMin, &app);

  const SourceRow* r = app.sources().find(SourceKind::Fm, 9230);
  const int64_t err = app.displayState().utc_us - sim.true_utc_us;
  AT_CHECK(r != nullptr);
  AT_CHECK(app.sources().rating(*r) == Rating::Red);
  AT_CHECK(err < 10 * kS && err > -10 * kS);
}

// FM that has never delivered clock time here gets two dwells at power-on,
// then HF — not the full five-minute hunt plus a fifteen-minute wait, which is
// what the owner's radio did at a QTH where FM had already proven useless.
AT_TEST(app_unproven_fm_hands_over_to_wwv_within_minutes) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {10000};
  FakeStation mute{9230, 0x986D, false, 0};   // on the air, no clock time
  sim.rds.stations = {mute};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9230};
  app.setFmStations(fm, 1);
  app.begin();

  const int64_t start = sim.clock.mono_us;
  bool listening = false;
  for (int i = 0; i < 10 * 60 && !listening; ++i) {
    sim.advance(kS, &app);
    listening = app.scheduler().phase() == Phase::Listening;
  }
  AT_CHECK(listening);
  AT_CHECK(sim.clock.mono_us - start <= 3 * kMin);
}

// A phone's clock is worth about a second, not a wristwatch's five, and the
// set says so.
AT_TEST(app_phone_set_claims_about_a_second) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  app.begin();
  app.setManualUtc(sim.true_utc_us, 1000000);
  const int64_t now = sim.clock.mono_us;
  AT_CHECK(app.arbiter().isSet());
  AT_CHECK(app.arbiter().uncertaintyUs(now) <= 1100000);
}

// One honest FM station sets the clock and it is Yellow. The owner's radio
// then sat Yellow all day, trying WWV three minutes an hour. Until something
// confirms the time, the windows stay frequent, and WWV turns it Green.
AT_TEST(app_yellow_keeps_listening_until_wwv_confirms) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation honest{10470, 0x6E47, true, 0};
  sim.rds.stations = {honest};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10470};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(3 * kMin, &app);
  AT_CHECK(app.displayState().synced);
  AT_CHECK(!app.displayState().confirmed);   // Yellow: one station's word

  bool confirmed = false;
  for (int i = 0; i < 30 && !confirmed; ++i) {
    sim.advance(kMin, &app, 10000);
    confirmed = app.displayState().confirmed;
  }
  AT_CHECK(confirmed);
  AT_CHECK((app.displayState().sources & kSrcWwv) != 0);
}

// Two single stations that disagree: one is wrong, and nothing yet says which,
// so neither may be marked Red. On the owner's radio 106.1 ran about 3 s slow,
// set the clock, and then convicted 98.9 — which was the right one.
AT_TEST(app_one_unconfirmed_station_cannot_convict_another) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation slow{10610, 0x829D, true, 3 * kS};
  FakeStation right{9890, 0x8B94, true, 0};
  sim.rds.stations = {slow, right};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10610, 9890};
  app.setFmStations(fm, 2);
  app.begin();
  sim.advance(20 * kMin, &app);

  const SourceRow* s = app.sources().find(SourceKind::Fm, 10610);
  const SourceRow* r = app.sources().find(SourceKind::Fm, 9890);
  AT_CHECK(s != nullptr && r != nullptr);
  AT_CHECK(app.sources().rating(*s) != Rating::Red);
  AT_CHECK(app.sources().rating(*r) != Rating::Red);
  AT_CHECK(!app.displayState().confirmed);
}

// The same two stations with WWV on the air: WWV breaks the tie. The clock
// ends on the right time, the slow station is Red, and the right one stands.
AT_TEST(app_wwv_breaks_a_tie_between_disagreeing_stations) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {5000, 10000, 15000};
  FakeStation slow{10610, 0x829D, true, 3 * kS};
  FakeStation right{9890, 0x8B94, true, 0};
  sim.rds.stations = {slow, right};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10610, 9890};
  app.setFmStations(fm, 2);
  app.begin();
  sim.advance(90 * kMin, &app, 10000);

  const SourceRow* s = app.sources().find(SourceKind::Fm, 10610);
  const SourceRow* r = app.sources().find(SourceKind::Fm, 9890);
  AT_CHECK(s != nullptr && r != nullptr);
  AT_CHECK(app.sources().rating(*s) == Rating::Red);
  AT_CHECK(app.sources().rating(*r) != Rating::Red);
  AT_CHECK(app.displayState().confirmed);
  AT_CHECK(iabs(sim.clockErrorUs(app)) < 500000);
}

// One honest FM station sets the clock at power-on: Yellow. After one short
// turn for other FM stations to agree, the first WWV window must follow — not
// fifteen minutes later; the owner stood at the big antenna for eight minutes
// hearing only FM.
AT_TEST(app_yellow_at_power_on_listens_for_wwv_within_minutes) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {10000};
  FakeStation honest{10610, 0x829D, true, 0};
  sim.rds.stations = {honest};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {10610};
  app.setFmStations(fm, 1);
  app.begin();

  const int64_t start = sim.clock.mono_us;
  bool listening = false;
  for (int i = 0; i < 10 * 60 && !listening; ++i) {
    sim.advance(kS, &app);
    listening = app.scheduler().phase() == Phase::Listening;
  }
  AT_CHECK(listening);
  AT_CHECK(app.arbiter().hasSourceFix());           // FM did set it first
  AT_CHECK(sim.clock.mono_us - start <= 5 * kMin);  // and HF within minutes
}

// The operator presses Listen Now while the radio is still hunting FM at
// power-on. The tuner goes to HF within seconds, not after the hunt.
AT_TEST(app_listen_now_during_the_fm_hunt_starts_wwv) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  sim.wwv.propagating_bands = {10000};
  FakeStation mute{9230, 0x986D, false, 0};
  sim.rds.stations = {mute};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9230};
  app.setFmStations(fm, 1);
  app.begin();

  sim.advance(30 * kS, &app);
  AT_CHECK(app.scheduler().phase() == Phase::Acquiring);
  app.operatorListenNow();
  sim.advance(5 * kS, &app);
  AT_CHECK(app.scheduler().phase() == Phase::Listening);
  AT_CHECK(sim.wwv.isRunning());
  AT_CHECK(!sim.adc_wifi_conflict);
}

// A laptop is never handed time that nothing has confirmed. One station, five
// minutes slow like 92.3 on the owner's dial, sets the radio's clock — but NTP
// carries the alarm flag until a different source agrees.
AT_TEST(app_ntp_refuses_unconfirmed_time) {
  Sim sim;
  sim.true_utc_us = startUtcUs();
  FakeStation slow{9230, 0x986D, true, 5 * kMin};
  sim.rds.stations = {slow};

  AppConfig cfg;
  cfg.auto_survey = false;
  AirTimeApp app(sim.deps(), cfg);
  const int32_t fm[] = {9230};
  app.setFmStations(fm, 1);
  app.begin();
  sim.advance(10 * kMin, &app);
  AT_CHECK(app.arbiter().isSet());

  uint8_t req[kNtpPacketSize], resp[kNtpPacketSize];
  makeNtpRequest(req);
  AT_CHECK(app.handleNtpRequest(req, sizeof(req), 0xC0A80402, resp));
  AT_CHECK_EQ((resp[0] >> 6) & 0x03, kNtpLeapAlarm);
  AT_CHECK_EQ(resp[1], kNtpStratumUnsync);
}
