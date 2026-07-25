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
  int64_t rds_report_ttl_us = 10LL * 60 * 1000000;   // stale stations stop voting
  int64_t rds_submit_interval_us = 30LL * 1000000;   // don't spam the arbiter
  int64_t fm_dwell_us = 20LL * 1000000;              // per-station scan dwell
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

 private:
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
