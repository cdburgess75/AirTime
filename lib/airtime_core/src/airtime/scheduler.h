#pragma once
//
// Acquisition & listen scheduler — PLAN.md §5 ("Serve-mostly with boot-time
// acquisition") plus the WWV band stepping of §4.
//
// This is the runtime brain: it decides, at every instant, whether WiFi is up,
// whether we are listening to HF, and which WWV band to sit on. The firmware
// adapters do nothing but obey the Directive it returns.
//
// It exists as pure logic because the single most dangerous constraint in the
// whole project is a scheduling decision:
//
//     ADC2 (IO11) CANNOT BE READ WHILE WIFI IS ACTIVE  (PLAN.md §2)
//
// so `wifi_up` and `wwv_listening` must never be true at the same time. That is
// an invariant of this state machine, and it is unit-tested rather than
// discovered on hardware.
//
// Phases (§5):
//   Acquiring — every power-on. WiFi DOWN; parallel hunt: RDS scan/vote AND WWV
//               band-step. Leaves on the first credible fix, or on timeout.
//   Serving   — SoftAP + NTP up, RDS re-sync continues in the background
//               (no WiFi conflict).
//   Listening — brief scheduled WWV window each hour; WiFi torn down for the
//               window, NTP clients coast through it.

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

enum class Phase { Acquiring, Serving, Listening };

struct SchedulerConfig {
  int64_t acquire_timeout_us = 5LL * 60 * 1000000;   // §5 default, configurable
  int64_t listen_interval_us = 60LL * 60 * 1000000;  // hourly listen window
  int64_t listen_duration_us = 3LL * 60 * 1000000;   // long enough for 2-3 markers
  // §4: dwell >= 2 min per band. This is a dwell on a SILENT band — the
  // rotation holds still once a band produces a marker (see onWwvMarker), so
  // the step never lands mid-measurement.
  int64_t band_dwell_us = 2LL * 60 * 1000000;
  bool exit_listen_on_fix = true;  // got what we came for; resume serving early

  // ── The unseeded cadence ────────────────────────────────────────────────
  // Before any source has fixed the clock, the listen windows ARE the
  // acquisition: the device is serving an NTP answer flagged unusable, so
  // there is nothing for the windows to interrupt and everything for them to
  // find. Windows come sooner and run longer — long enough for the 100 Hz
  // timecode to hunt the frame (up to a minute) and read two whole frames on
  // one band, which is what the longer dwell guarantees room for. The moment
  // any fix lands, setSeeded() flips the cadence back to the serving-first
  // numbers above.
  int64_t listen_interval_unseeded_us = 15LL * 60 * 1000000;
  int64_t listen_duration_unseeded_us = 8LL * 60 * 1000000;
  int64_t band_dwell_unseeded_us = 4LL * 60 * 1000000;
};

// What the hardware adapters should be doing right now.
struct Directive {
  Phase phase = Phase::Acquiring;
  bool wifi_up = false;
  bool wwv_listening = false;
  bool rds_scanning = false;
  int32_t wwv_band_khz = 0;  // 0 when not listening
};

// Per-band propagation log (§4: "Log per-band success + SNR to learn local
// propagation (expect 5 MHz at night, 10/15 by day)").
struct BandStats {
  int32_t khz = 0;
  int attempts = 0;
  int successes = 0;
  real best_snr = 0.0f;
};

class Scheduler {
 public:
  static constexpr std::size_t kMaxBands = 5;

  explicit Scheduler(const SchedulerConfig& cfg = SchedulerConfig{});

  // Replace the WWV band rotation (default 5/10/15 MHz; §4 allows adding
  // 2.5/20 MHz). Silently caps at kMaxBands.
  void setBands(const int32_t* khz, std::size_t n);

  // Begin at power-on (WiFi down, hunting). Safe to omit: the first tick starts.
  void start(int64_t mono_us);

  // Drive the machine. Call from the main loop; returns the current directive.
  Directive tick(int64_t mono_us);

  // A WWV minute marker was DETECTED on the current band. Credits the band's
  // propagation log (§4) and pins the rotation to this band for the rest of
  // the window — detection is evidence about the BAND even when the arbiter
  // rejects the implied correction.
  void onWwvMarker(real snr = 0.0f);

  // A WWV fix was ACCEPTED by the arbiter: (by default) ends the listen window
  // early. Call only for accepted fixes. A rejected large correction must
  // leave the window OPEN — the next minute's marker is the only evidence
  // that can corroborate it, and closing early discards it (first-day field
  // bug: three genuine markers detected, zero applied).
  void onWwvFix(int64_t mono_us);

  // An RDS fix landed (can happen in any phase; RDS never needs WiFi down).
  void onRdsFix(int64_t mono_us);

  // Restore what a previous power-on learned about a band. Matched by
  // FREQUENCY rather than index, so a build whose band list has been reordered
  // or extended cannot credit the wrong band. Returns false if this radio does
  // not currently rotate through that frequency.
  bool seedBandStats(int32_t khz, int successes, real best_snr);

  // Operator overrides from the encoder menu (§5).
  void requestListenNow();
  void requestServeNow();

  // Whether a real source has fixed the clock since power-on — the app relays
  // arbiter.hasSourceFix() every loop. Selects between the serving-first and
  // unseeded cadences above. Defaults to seeded, so a caller that never says
  // gets exactly the old behaviour.
  void setSeeded(bool s) { seeded_ = s; }
  bool seeded() const { return seeded_; }

  Phase phase() const { return phase_; }
  bool hasFix() const { return has_fix_; }
  int32_t currentBandKhz() const;
  std::size_t bandCount() const { return band_count_; }
  const BandStats& bandStats(std::size_t i) const { return bands_[i]; }
  // Band with the most successes so far (ties -> current rotation position).
  std::size_t preferredBandIndex() const;

 private:
  Directive directive() const;
  void enterServing(int64_t mono_us);
  void enterListening(int64_t mono_us);
  void maybeStepBand(int64_t mono_us);
  int64_t listenIntervalUs() const {
    return seeded_ ? cfg_.listen_interval_us : cfg_.listen_interval_unseeded_us;
  }
  int64_t listenDurationUs() const {
    return seeded_ ? cfg_.listen_duration_us : cfg_.listen_duration_unseeded_us;
  }
  int64_t bandDwellUs() const {
    return seeded_ ? cfg_.band_dwell_us : cfg_.band_dwell_unseeded_us;
  }

  SchedulerConfig cfg_;
  Phase phase_ = Phase::Acquiring;
  bool started_ = false;
  bool has_fix_ = false;
  bool seeded_ = true;

  int64_t acquire_start_ = 0;
  int64_t listen_start_ = 0;
  int64_t last_listen_end_ = 0;
  int64_t band_start_ = 0;

  bool want_listen_ = false;
  bool want_serve_ = false;
  bool band_productive_ = false;  // current band has yielded a marker

  BandStats bands_[kMaxBands];
  std::size_t band_count_ = 0;
  std::size_t band_idx_ = 0;
};

}  // namespace airtime
