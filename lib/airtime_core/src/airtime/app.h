#pragma once
//
// AirTimeApp — the wiring that turns the core modules into a device.
//
// It owns the arbiter, scheduler, RDS voter and WWV marker detector, and drives
// them from a single polled loop against the hardware seam (hal.h). Because the
// seam is injected, the entire device runs on the host against fakes — see
// test/fakes.h and test/test_app.cpp, where a simulated radio with a drifting
// crystal disciplines itself and answers NTP correctly.
//
// The firmware's main loop is expected to be roughly:
//
//     app.begin();
//     for (;;) { app.loop(); /* plus: render display, service UDP */ }

#include <cstddef>
#include <cstdint>

#include "arbiter.h"
#include "display.h"
#include "hal.h"
#include "rds_ct.h"
#include "scheduler.h"
#include "sntp.h"
#include "station_vote.h"
#include "types.h"
#include "wwv_marker.h"

namespace airtime {

struct AppConfig {
  ArbiterConfig arbiter;
  SchedulerConfig scheduler;
  WwvMarkerConfig marker;

  // Fixed latency of the receive chain (SI4732 DSP group delay + amp + ADC).
  // PLAN.md §4: measured once on hardware; the one number host tests cannot know.
  int64_t wwv_calibration_us = 0;

  int64_t wwv_uncertainty_us = 30000;    // ±30 ms — marker edge detection
  int64_t rds_uncertainty_us = 250000;   // ±250 ms — RDS CT is coarse
  int64_t rds_vote_tolerance_us = 400000;
  // A station's report must outlive a full scan cycle, or stations drop out of
  // the voter before the rotation returns to them and voting degrades to one
  // source. Keep this > station_count * fm_dwell_us.
  int64_t rds_report_ttl_us = 15LL * 60 * 1000000;
  // How often RDS may steer the clock. Weighting alone does not settle the
  // contest between sources, because influence is gain x RATE: RDS at ~48
  // fixes/hour still out-pulls WWV at 1/hour even when each RDS fix is scaled to
  // a few percent. And RDS error is systematic (a given station is consistently
  // early or late), so repeated samples do not average it away the way the
  // Kalman blend assumes.
  //
  // So once the clock is already better than RDS's own accuracy, RDS is
  // throttled hard. Its §4 job is date and coarse time, not phase — beyond that
  // point more RDS contributes only its bias.
  int64_t rds_submit_interval_us = 30LL * 1000000;
  int64_t rds_submit_interval_disciplined_us = 10LL * 60 * 1000000;
  int64_t rds_disciplined_below_us = 150000;  // "better than RDS can tell us"

  // Per-station scan dwell. MUST exceed the RDS clock-time repeat interval —
  // group 4A is transmitted about once a MINUTE, so a shorter dwell only
  // sometimes catches one. Worse, a dwell that divides evenly into 60 s tunes
  // the *same* station at every minute boundary, so only one station is ever
  // heard and the multi-station voting §4 calls mandatory silently never
  // happens. (Observed with a 20 s dwell: the voter held exactly one report
  // forever, and the clock locked onto a station that was 7 s wrong while
  // reporting itself synced.) 75 s clears the minute with margin and is not a
  // divisor of it.
  int64_t fm_dwell_us = 75LL * 1000000;
  int64_t drift_save_interval_us = 60LL * 60 * 1000000;
  int64_t restore_uncertainty_us = 3600LL * 1000000; // warm boot is a memory
  int64_t ntp_client_window_us = 5LL * 60 * 1000000;
  int64_t source_recent_us = 2LL * 3600 * 1000000;   // for the "RDS+WWV" display
};

struct AppDeps {
  IMonotonicClock* clock = nullptr;
  IRdsSource* rds = nullptr;
  IWwvSampler* wwv = nullptr;
  IWiFiControl* wifi = nullptr;
  ITimeStore* store = nullptr;  // optional
};

class AirTimeApp {
 public:
  static constexpr std::size_t kMaxStations = 8;

  AirTimeApp(const AppDeps& deps, const AppConfig& cfg = AppConfig{});

  // FM frequencies to rotate through while hunting RDS clock-time. Populated
  // from the Milestone 1 station survey.
  void setFmStations(const int32_t* khz, std::size_t n);

  void begin();
  void loop();

  // Answer an NTP request. Returns false if it is not a valid client request.
  bool handleNtpRequest(const uint8_t* req, std::size_t len, uint32_t client_id,
                        uint8_t* resp48);

  // Operator actions (§5).
  void operatorListenNow() { sched_.requestListenNow(); }
  void operatorServeNow() { sched_.requestServeNow(); }
  void operatorConfirmStep() { arbiter_.setOperatorConfirm(true); }
  void operatorSetTime(int64_t utc_us);

  DisplayState displayState() const;
  int64_t utcNow() const;

  const Arbiter& arbiter() const { return arbiter_; }
  const Scheduler& scheduler() const { return sched_; }
  const Directive& directive() const { return directive_; }
  const WwvMarkerDetector& wwvMarker() const { return marker_; }

 private:
  // The scheduler's directive, adjusted for what only the app knows. Today
  // that is one rule: no WWV listening before the arbiter has a time at all.
  // WWV markers are ±30 s ambiguous — pollWwv discards them unseeded — and on
  // the real single-tuner radio a pre-seed listen window would also starve
  // the RDS acquisition that CAN seed. (A warm boot restores the arbiter, so
  // WWV listening is available immediately then — exactly right: the minute
  // is known, only phase needs refining.)
  Directive effectiveDirective(Directive d) const;
  void applyDirective(const Directive& d);
  void pollRds(int64_t now);
  void pollWwv(int64_t now);
  void submitRdsVote(int64_t now);
  void persist(int64_t now, bool force);
  void noteAccepted(Source s, int64_t now);
  uint8_t recentSourceMask(int64_t now) const;

  AppDeps deps_;
  AppConfig cfg_;

  Arbiter arbiter_;
  Scheduler sched_;
  StationVoter voter_;
  WwvMarkerDetector marker_;
  ClientCounter clients_;

  Directive directive_;

  int32_t stations_[kMaxStations] = {};
  std::size_t station_count_ = 0;
  std::size_t station_idx_ = 0;
  int64_t fm_dwell_start_ = 0;

  int32_t tuned_wwv_khz_ = 0;
  bool have_new_ct_ = false;
  int64_t last_rds_submit_ = 0;
  int64_t last_persist_ = 0;

  bool ever_synced_ = false;
  int64_t last_sync_mono_ = 0;
  int64_t last_sync_utc_ = 0;
  Source last_source_ = Source::None;
  int64_t source_seen_[4] = {0, 0, 0, 0};  // indexed by Source
  bool source_ever_[4] = {false, false, false, false};
};

}  // namespace airtime
