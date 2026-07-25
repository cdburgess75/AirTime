#include "app.h"

namespace airtime {

AirTimeApp::AirTimeApp(const AppDeps& deps, const AppConfig& cfg)
    : deps_(deps),
      cfg_(cfg),
      arbiter_(cfg.arbiter),
      sched_(cfg.scheduler),
      marker_(cfg.marker) {}

void AirTimeApp::setFmStations(const int32_t* khz, std::size_t n) {
  if (khz == nullptr) return;
  if (n > kMaxStations) n = kMaxStations;
  for (std::size_t i = 0; i < n; ++i) stations_[i] = khz[i];
  station_count_ = n;
  station_idx_ = 0;
}

void AirTimeApp::begin() {
  const int64_t now = deps_.clock->nowUs();

  // Order matters: seed the learned rate first, because restoring the clock
  // preserves the rate correction but not the other way round.
  if (deps_.store != nullptr) {
    double ppm = 0.0;
    if (deps_.store->loadDriftPpm(&ppm)) arbiter_.seedDriftPpm(now, ppm);

    int64_t utc = 0;
    if (deps_.store->loadLastUtc(&utc)) {
      // A memory, not a measurement — restored unsynced (§5).
      arbiter_.restore(now, utc, cfg_.restore_uncertainty_us);
    }
  }

  sched_.start(now);
  fm_dwell_start_ = now;
  last_persist_ = now;
  if (station_count_ > 0) deps_.rds->tuneKhz(stations_[0]);

  directive_ = sched_.tick(now);
  applyDirective(directive_);
}

void AirTimeApp::applyDirective(const Directive& d) {
  // THE ordering rule (PLAN.md §2): ADC2 and WiFi can never be live together.
  // Always release before acquiring — stop the sampler and drop WiFi first, then
  // bring up whatever the new directive wants.
  if (!d.wwv_listening && deps_.wwv->isRunning()) deps_.wwv->stop();
  if (!d.wifi_up && deps_.wifi->isUp()) deps_.wifi->tearDown();

  if (d.wifi_up && !deps_.wifi->isUp()) deps_.wifi->bringUp();

  if (d.wwv_listening) {
    if (d.wwv_band_khz != tuned_wwv_khz_) {
      deps_.wwv->tuneKhz(d.wwv_band_khz);
      tuned_wwv_khz_ = d.wwv_band_khz;
      marker_.reset();  // new band, new noise floor
    }
    if (!deps_.wwv->isRunning()) deps_.wwv->start();
  }
}

void AirTimeApp::loop() {
  const int64_t now = deps_.clock->nowUs();

  directive_ = sched_.tick(now);
  applyDirective(directive_);

  pollRds(now);
  if (directive_.wwv_listening) pollWwv(now);

  persist(now, /*force=*/false);
}

void AirTimeApp::pollRds(int64_t now) {
  RdsGroup g;
  while (deps_.rds->poll(&g)) {
    RdsClockTime t;
    if (!decodeRdsClockTime(g.a, g.b, g.c, g.d, &t)) continue;

    // Block A is the station's PI code — its identity for voting purposes.
    CtReport r;
    r.pi = g.a;
    r.asserted_utc_us = t.utc_epoch_s * 1000000;
    r.rx_monotonic_us = g.mono_us;
    voter_.add(r);
    have_new_ct_ = true;
  }

  // Rotate the FM scan so every receivable station gets a chance to report.
  if (station_count_ > 1 && (now - fm_dwell_start_) >= cfg_.fm_dwell_us) {
    station_idx_ = (station_idx_ + 1) % station_count_;
    deps_.rds->tuneKhz(stations_[station_idx_]);
    fm_dwell_start_ = now;
  }

  voter_.prune(now - cfg_.rds_report_ttl_us);

  if (have_new_ct_ && (now - last_rds_submit_) >= cfg_.rds_submit_interval_us) {
    submitRdsVote(now);
  }
}

void AirTimeApp::submitRdsVote(int64_t now) {
  const VoteResult vr = voter_.vote(cfg_.rds_vote_tolerance_us);
  if (vr.total_reports == 0) return;

  have_new_ct_ = false;
  last_rds_submit_ = now;

  // The vote yields a constant monotonic->UTC mapping, so UTC at `now` is just
  // the consensus offset projected forward.
  //
  // Known, measured bias: the median mixes reports of differing ages, so the
  // consensus mapping lags by roughly (mean report age x drift rate). Simulated
  // at 28 ppm with ~60 s station spacing this is a steady -840 µs — 0.4% of the
  // ±250 ms we already declare for RDS, and immaterial to FT8. Voting is kept
  // median-based because its job (§4) is outlier rejection, not precision; WWV
  // supplies the precision.
  TimeFix f;
  f.source = Source::Rds;
  f.mono_us = now;
  f.utc_us = vr.offset_us + now;
  f.uncertainty_us = cfg_.rds_uncertainty_us;
  f.independent_support = vr.agreeing_stations;

  const ArbiterUpdate u = arbiter_.update(f);
  if (u.action != Action::Rejected) noteAccepted(Source::Rds, now);
}

void AirTimeApp::pollWwv(int64_t now) {
  int64_t t = 0;
  real p = 0;
  WwvMarker m;
  while (deps_.wwv->nextPower(&t, &p)) {
    if (!marker_.process(t, p, &m)) continue;

    // WWV carries no date (PLAN.md §3): it can only pull an already-roughly-right
    // clock onto the exact minute boundary. It cannot cold-start one.
    if (!arbiter_.isSet()) continue;

    const int64_t clock_utc = arbiter_.utcAt(m.leading_edge_us);
    int64_t off = 0;
    if (!wwvPhaseCorrection(clock_utc, cfg_.wwv_calibration_us, &off)) continue;

    TimeFix f;
    f.source = Source::Wwv;
    f.mono_us = m.leading_edge_us;
    f.utc_us = clock_utc + off;
    f.uncertainty_us = cfg_.wwv_uncertainty_us;
    f.independent_support = 1;

    const ArbiterUpdate u = arbiter_.update(f);
    if (u.action != Action::Rejected) noteAccepted(Source::Wwv, now);

    sched_.onWwvFix(now, m.peak_power);
  }
}

void AirTimeApp::noteAccepted(Source s, int64_t now) {
  const int i = static_cast<int>(s);
  if (i >= 0 && i < 4) {
    source_seen_[i] = now;
    source_ever_[i] = true;
  }
  last_source_ = s;
  ever_synced_ = true;
  last_sync_mono_ = now;
  last_sync_utc_ = arbiter_.utcAt(now);
  if (s == Source::Rds) sched_.onRdsFix(now);
}

void AirTimeApp::operatorSetTime(int64_t utc_us) {
  const int64_t now = deps_.clock->nowUs();
  arbiter_.setOperatorConfirm(true);  // a manual set is its own authorization
  TimeFix f;
  f.source = Source::Manual;
  f.mono_us = now;
  f.utc_us = utc_us;
  f.uncertainty_us = 500000;  // human reaction time, generously
  f.independent_support = 1;
  const ArbiterUpdate u = arbiter_.update(f);
  if (u.action != Action::Rejected) noteAccepted(Source::Manual, now);
}

void AirTimeApp::persist(int64_t now, bool force) {
  if (deps_.store == nullptr) return;
  if (!force && (now - last_persist_) < cfg_.drift_save_interval_us) return;
  last_persist_ = now;
  if (arbiter_.drift().hasEstimate()) deps_.store->saveDriftPpm(arbiter_.ratePpm());
  if (arbiter_.isSet()) deps_.store->saveLastUtc(arbiter_.utcAt(now));
}

uint8_t AirTimeApp::recentSourceMask(int64_t now) const {
  uint8_t mask = 0;
  const Source all[] = {Source::Rds, Source::Wwv, Source::Manual};
  for (Source s : all) {
    const int i = static_cast<int>(s);
    if (source_ever_[i] && (now - source_seen_[i]) <= cfg_.source_recent_us) {
      mask |= sourceBit(s);
    }
  }
  return mask;
}

int64_t AirTimeApp::utcNow() const {
  return arbiter_.utcAt(deps_.clock->nowUs());
}

DisplayState AirTimeApp::displayState() const {
  const int64_t now = deps_.clock->nowUs();
  DisplayState st;
  st.clock_valid = arbiter_.isSet();
  st.synced = arbiter_.isSynced(now);
  st.ever_synced = ever_synced_;
  st.utc_us = arbiter_.utcAt(now);
  st.uncertainty_us = arbiter_.uncertaintyUs(now);
  st.since_sync_us = ever_synced_ ? now - last_sync_mono_ : 0;
  st.sources = recentSourceMask(now);
  st.ntp_clients = clients_.countActive(now, cfg_.ntp_client_window_us);
  st.phase = sched_.phase();
  return st;
}

bool AirTimeApp::handleNtpRequest(const uint8_t* req, std::size_t len,
                                  uint32_t client_id, uint8_t* resp48) {
  const int64_t now = deps_.clock->nowUs();

  SntpServerState st;
  st.synced = arbiter_.isSynced(now);
  st.uncertainty_us = arbiter_.uncertaintyUs(now);
  st.last_sync_utc_us = last_sync_utc_;
  st.source = last_source_;

  const int64_t utc = arbiter_.utcAt(now);
  if (!handleSntpRequest(req, len, utc, utc, st, resp48)) return false;

  clients_.touch(client_id, now);
  return true;
}

}  // namespace airtime
