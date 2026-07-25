#include <cstdint>

#include "airtime/scheduler.h"
#include "test_framework.h"

using namespace airtime;

namespace {
constexpr int64_t kS = 1000000;       // one second in µs
constexpr int64_t kMin = 60 * kS;
}  // namespace

// Power-on begins with WiFi DOWN and both hunts running (§5).
AT_TEST(sched_boot_is_acquiring_wifi_down) {
  Scheduler s;
  const Directive d = s.tick(0);
  AT_CHECK(d.phase == Phase::Acquiring);
  AT_CHECK(!d.wifi_up);
  AT_CHECK(d.wwv_listening);
  AT_CHECK(d.rds_scanning);
  AT_CHECK_EQ(d.wwv_band_khz, 5000);
}

// THE safety invariant (PLAN.md §2): ADC2 cannot be read while WiFi is active,
// so wifi_up and wwv_listening must never be simultaneously true — in any phase,
// under any sequence of events.
AT_TEST(sched_never_wifi_and_adc_together) {
  Scheduler s;
  s.start(0);
  for (int64_t t = 0; t < 8 * 3600 * kS; t += 17 * kS) {
    const Directive d = s.tick(t);
    AT_CHECK(!(d.wifi_up && d.wwv_listening));
    if (t == 90 * kS) s.onRdsFix(t);            // fix during acquisition
    if (t == 2 * 3600 * kS) s.requestListenNow();
    if (t == 4 * 3600 * kS) s.onWwvFix(t, 12.0f);
    if (t == 5 * 3600 * kS) s.requestServeNow();
  }
}

// A fix during acquisition promotes us to serving (§5: "on fix OR timeout").
AT_TEST(sched_fix_starts_serving) {
  Scheduler s;
  s.start(0);
  s.tick(10 * kS);
  s.onRdsFix(10 * kS);
  const Directive d = s.tick(11 * kS);
  AT_CHECK(d.phase == Phase::Serving);
  AT_CHECK(d.wifi_up);
  AT_CHECK(!d.wwv_listening);
  AT_CHECK(d.rds_scanning);  // RDS keeps running while serving
}

// With no fix at all we still start serving at the timeout, honestly unsynced.
AT_TEST(sched_timeout_starts_serving) {
  Scheduler s;
  s.start(0);
  AT_CHECK(s.tick(4 * kMin).phase == Phase::Acquiring);
  const Directive d = s.tick(5 * kMin + 1);
  AT_CHECK(d.phase == Phase::Serving);
  AT_CHECK(d.wifi_up);
  AT_CHECK(!s.hasFix());
}

// The hourly listen window tears WiFi down, then serving resumes afterwards.
AT_TEST(sched_hourly_listen_window) {
  SchedulerConfig cfg;
  cfg.exit_listen_on_fix = false;  // isolate the timing behavior
  Scheduler s(cfg);
  s.start(0);
  s.onRdsFix(0);
  s.tick(1 * kS);
  AT_CHECK(s.phase() == Phase::Serving);

  AT_CHECK(s.tick(59 * kMin).phase == Phase::Serving);   // not yet
  const Directive d = s.tick(60 * kMin + kS);            // due
  AT_CHECK(d.phase == Phase::Listening);
  AT_CHECK(!d.wifi_up);       // NTP clients coast through the window
  AT_CHECK(d.wwv_listening);

  AT_CHECK(s.tick(62 * kMin).phase == Phase::Listening); // still within 3 min
  AT_CHECK(s.tick(64 * kMin).phase == Phase::Serving);   // window over
}

// A WWV fix ends the window early — no reason to keep NTP down.
AT_TEST(sched_wwv_fix_ends_window_early) {
  Scheduler s;
  s.start(0);
  s.onRdsFix(0);
  s.tick(kS);
  s.tick(61 * kMin);
  AT_CHECK(s.phase() == Phase::Listening);
  s.onWwvFix(61 * kMin + 30 * kS, 9.0f);
  AT_CHECK(s.phase() == Phase::Serving);
  AT_CHECK(s.tick(61 * kMin + 31 * kS).wifi_up);
}

// Operator overrides from the encoder (§5).
AT_TEST(sched_operator_overrides) {
  SchedulerConfig cfg;
  cfg.exit_listen_on_fix = false;
  Scheduler s(cfg);
  s.start(0);
  s.onRdsFix(0);
  s.tick(kS);
  AT_CHECK(s.phase() == Phase::Serving);

  s.requestListenNow();                       // "listen now"
  AT_CHECK(s.tick(2 * kS).phase == Phase::Listening);

  s.requestServeNow();                        // "serve now"
  AT_CHECK(s.tick(3 * kS).phase == Phase::Serving);
}

// Band stepping rotates on the dwell interval (§4: >= 2 min per band).
AT_TEST(sched_band_stepping) {
  Scheduler s;
  s.start(0);
  AT_CHECK_EQ(s.tick(0).wwv_band_khz, 5000);
  AT_CHECK_EQ(s.tick(1 * kMin).wwv_band_khz, 5000);   // still dwelling
  AT_CHECK_EQ(s.tick(2 * kMin).wwv_band_khz, 10000);  // stepped
  AT_CHECK_EQ(s.tick(4 * kMin).wwv_band_khz, 15000);
}

// Per-band success/SNR is logged, and the band that works is preferred next time
// (§4: learn local propagation).
AT_TEST(sched_learns_preferred_band) {
  SchedulerConfig cfg;
  cfg.exit_listen_on_fix = false;
  Scheduler s(cfg);
  s.start(0);

  // Succeed on 10 MHz (index 1) during acquisition.
  s.tick(2 * kMin);
  AT_CHECK_EQ(s.currentBandKhz(), 10000);
  s.onWwvFix(2 * kMin, 14.0f);
  AT_CHECK_EQ(s.bandStats(1).successes, 1);
  AT_CHECK_NEAR(s.bandStats(1).best_snr, 14.0, 1e-6);
  AT_CHECK_EQ(s.preferredBandIndex(), 1u);

  s.tick(3 * kMin);  // -> Serving (we have a fix)
  AT_CHECK(s.phase() == Phase::Serving);

  // The next listen window opens on the proven band rather than restarting at 5 MHz.
  const Directive d = s.tick(64 * kMin);
  AT_CHECK(d.phase == Phase::Listening);
  AT_CHECK_EQ(d.wwv_band_khz, 10000);
}

// The band rotation is configurable (§4 allows adding 2.5/20 MHz).
AT_TEST(sched_custom_bands) {
  Scheduler s;
  const int32_t bands[] = {2500, 5000, 10000, 15000, 20000};
  s.setBands(bands, 5);
  AT_CHECK_EQ(s.bandCount(), 5u);
  s.start(0);
  AT_CHECK_EQ(s.tick(0).wwv_band_khz, 2500);
  AT_CHECK_EQ(s.tick(2 * kMin).wwv_band_khz, 5000);
}
