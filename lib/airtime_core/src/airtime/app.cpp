#include "app.h"

namespace airtime {

AirTimeApp::AirTimeApp(const AppDeps& deps, const AppConfig& cfg)
    : deps_(deps),
      cfg_(cfg),
      arbiter_(cfg.arbiter),
      sched_(cfg.scheduler),
      bias_(cfg.station_bias),
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

    // What this radio learned about the world around it. Both decoders ignore
    // anything they cannot fully verify, so a corrupt or older-format blob
    // costs a relearn rather than a fabricated correction.
    uint8_t blob[kStationBiasBlobMax > kBandStatsBlobMax ? kStationBiasBlobMax
                                                        : kBandStatsBlobMax];
    std::size_t got = 0;
    if (deps_.store->loadBlob(kBlobStationBias, blob, sizeof(blob), &got)) {
      decodeStationBias(blob, got, &bias_);
    }
    if (deps_.store->loadBlob(kBlobBandStats, blob, sizeof(blob), &got)) {
      decodeBandStats(blob, got, &sched_);
    }
  }

  sched_.start(now);
  fm_dwell_start_ = now;
  last_persist_ = now;
  if (station_count_ > 0) deps_.rds->tuneKhz(stations_[0]);

  directive_ = effectiveDirective(sched_.tick(now));
  applyDirective(directive_);
}

Directive AirTimeApp::effectiveDirective(Directive d) const {
  // See the declaration for the reasoning: unseeded, WWV can neither help
  // (markers are ±30 s ambiguous; pollWwv discards them) nor be afforded —
  // on the single-tuner radio it would starve the RDS path that CAN seed.
  //
  // The test is "has a real source spoken yet", NOT "is the clock set". A warm
  // boot sets the clock from NVS, and that memory is only as good as the
  // power-down was short — measured on the device: an hour stale. Trusting it
  // cost twice over. The tuner sat on AM for the whole 5-minute acquisition
  // (RDS cannot be read in AM mode, so nothing could end it early), which is
  // where "no WiFi for five minutes after boot" came from; and WWV was invited
  // to phase-lock a clock whose MINUTE was wrong, which a ±30 s marker cannot
  // detect and would have silently locked in.
  if (!arbiter_.hasSourceFix() && d.wwv_listening) {
    d.wwv_listening = false;
    d.wwv_band_khz = 0;
  }
  return d;
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

  directive_ = effectiveDirective(sched_.tick(now));
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

    // Block A is the station's PI code — its identity, for voting and for
    // remembering how late it runs.
    const int64_t asserted = t.utc_epoch_s * 1000000;
    // Compare against the clock AT RECEPTION, so the implied offset carries no
    // drift term. See the note on CtReport::reference_us — projecting to "now"
    // instead creates a bias proportional to the rate error, which blinds the
    // drift estimator to the very thing it is measuring.
    const int64_t reference = arbiter_.isSet() ? arbiter_.utcAt(g.mono_us) : g.mono_us;

    // WWV teaches RDS. Just after WWV has moved the clock, whatever a station
    // disagrees by IS its bias — so measure it, then stop. The window is what
    // keeps the lesson honest: measure indefinitely and the bias quietly
    // absorbs our own drift, and the station stops being an independent check.
    if (arbiter_.isSet() && have_wwv_accept_ &&
        (g.mono_us - last_wwv_accept_mono_) <= cfg_.station_bias_learn_window_us &&
        arbiter_.uncertaintyUs(g.mono_us) <= cfg_.station_bias_learn_below_us) {
      bias_.observe(g.a, reference - asserted, g.mono_us);
      learned_dirty_ = true;
    }

    CtReport r;
    r.pi = g.a;
    // Put the station back on time before it votes. Zero until the station has
    // been measured enough times to be trusted.
    r.asserted_utc_us = asserted + bias_.correction(g.a);
    r.reference_us = reference;
    r.rx_monotonic_us = g.mono_us;
    voter_.add(r);
    have_new_ct_ = true;
  }

  // Rotate the FM scan so every receivable station gets a chance to report —
  // but never while a WWV listen window owns the tuner. The fakes are two
  // independent radios; the device has ONE, and an ungated rotation here
  // yanked the chip from AM back to FM 75 s into every real listen window.
  if (!directive_.wwv_listening && station_count_ > 1 &&
      (now - fm_dwell_start_) >= cfg_.fm_dwell_us) {
    station_idx_ = (station_idx_ + 1) % station_count_;
    deps_.rds->tuneKhz(stations_[station_idx_]);
    fm_dwell_start_ = now;
  }

  voter_.prune(now - cfg_.rds_report_ttl_us);

  // Throttle RDS once we are already more accurate than it is; see AppConfig.
  const bool disciplined = arbiter_.isSet() &&
                           arbiter_.uncertaintyUs(now) < cfg_.rds_disciplined_below_us;
  const int64_t interval = disciplined ? cfg_.rds_submit_interval_disciplined_us
                                       : cfg_.rds_submit_interval_us;
  if (have_new_ct_ && (now - last_rds_submit_) >= interval) {
    submitRdsVote(now);
  }
}

void AirTimeApp::submitRdsVote(int64_t now) {
  const VoteResult vr = voter_.vote(cfg_.rds_vote_tolerance_us);
  if (vr.total_reports == 0) return;

  have_new_ct_ = false;
  last_rds_submit_ = now;

  // vr.offset_us is the consensus clock ERROR (see CtReport::reference_us), so
  // the asserted UTC at `now` is our own estimate plus that error. Once the
  // clock is set this is drift-free; before it is set the reports carry the raw
  // monotonic->UTC mapping, which is exactly what seeds it.
  TimeFix f;
  f.source = Source::Rds;
  f.mono_us = now;
  f.utc_us = (arbiter_.isSet() ? arbiter_.utcAt(now) : now) + vr.offset_us;
  f.uncertainty_us = cfg_.rds_uncertainty_us;
  f.independent_support = vr.agreeing_stations;

  const ArbiterUpdate u = arbiter_.update(f);

  rds_diag_.have = true;
  rds_diag_.offset_us = u.offset_us;
  rds_diag_.stations = vr.agreeing_stations;
  rds_diag_.accepted = u.action != Action::Rejected;

  if (u.action != Action::Rejected) noteAccepted(Source::Rds, now);
}

void AirTimeApp::pollWwv(int64_t now) {
  int64_t t = 0;
  real p = 0;
  WwvMarker m;
  while (deps_.wwv->nextPower(&t, &p)) {
    if (!marker_.process(t, p, &m)) continue;

    // A detection teaches the band table regardless of what the arbiter makes
    // of the implied correction — propagation is real either way.
    sched_.onWwvMarker(m.peak_power);
    learned_dirty_ = true;

    // WWV carries no date (PLAN.md §3): it can only pull an already-roughly-right
    // clock onto the exact minute boundary. It cannot cold-start one — and a
    // restored NVS time does not count as "roughly right": measured 65 minutes
    // stale, which no ±30 s marker can see. Wait for a source that knows the
    // date. (The arbiter refuses this too; belt and braces, because the cost of
    // getting it wrong is a confidently wrong clock.)
    if (!arbiter_.hasSourceFix()) continue;

    const int64_t clock_utc = arbiter_.utcAt(m.leading_edge_us);
    int64_t off = 0;
    if (!wwvPhaseCorrection(clock_utc, cfg_.wwv_calibration_us, &off)) continue;

    TimeFix f;
    f.source = Source::Wwv;
    f.mono_us = m.leading_edge_us;
    f.utc_us = clock_utc + off;
    f.uncertainty_us = cfg_.wwv_uncertainty_us;
    f.independent_support = 1;

    // Self-corroboration (§4 rule 3). On the single-tuner radio no other
    // source can second a big WWV correction inside the listen window — but
    // WWV seconds itself: if the previous minute's rejected marker implied
    // the same correction, this one is an independent transmission agreeing
    // within tolerance. See AppConfig::wwv_pair_* for why that agreement is
    // not coincidence.
    if (have_prev_wwv_) {
      const int64_t dt = m.leading_edge_us - prev_wwv_mono_;
      int64_t dof = off - prev_wwv_offset_us_;
      if (dof < 0) dof = -dof;
      if (dt >= cfg_.wwv_pair_min_dt_us && dt <= cfg_.wwv_pair_max_dt_us &&
          dof <= cfg_.wwv_pair_agree_us) {
        f.independent_support = 2;
      }
    }

    const ArbiterUpdate u = arbiter_.update(f);

    wwv_diag_.have = true;
    wwv_diag_.offset_us = off;
    wwv_diag_.corroborated = f.independent_support >= 2;
    wwv_diag_.accepted = u.action != Action::Rejected;

    if (u.action != Action::Rejected) {
      have_prev_wwv_ = false;  // consumed: the clock moved
      have_wwv_accept_ = true; // the teacher is in the room (see pollRds)
      last_wwv_accept_mono_ = m.leading_edge_us;
      noteAccepted(Source::Wwv, now);
      sched_.onWwvFix(now);    // only an ACCEPTED fix may end the window
    } else {
      have_prev_wwv_ = true;   // hold the window; next minute decides
      prev_wwv_mono_ = m.leading_edge_us;
      prev_wwv_offset_us_ = off;
    }
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

  // Learned state, written only when it has actually changed. Flash endures
  // ~100k cycles per sector and this runs for years; an unconditional hourly
  // write of a table that has not moved is wear for nothing.
  if (learned_dirty_) {
    uint8_t blob[kStationBiasBlobMax > kBandStatsBlobMax ? kStationBiasBlobMax
                                                         : kBandStatsBlobMax];
    std::size_t n = encodeStationBias(bias_, blob, sizeof(blob));
    if (n > 0) deps_.store->saveBlob(kBlobStationBias, blob, n);
    n = encodeBandStats(sched_, blob, sizeof(blob));
    if (n > 0) deps_.store->saveBlob(kBlobBandStats, blob, n);
    learned_dirty_ = false;
  }
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
  // What we serve is off by the un-slewed remainder too; say so (§5 honesty).
  st.uncertainty_us = arbiter_.uncertaintyUs(now) + arbiter_.pendingCorrectionUs(now);
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
  // Root dispersion must cover the correction still being slewed in: the client
  // is reading a clock we already know is that far out.
  st.uncertainty_us = arbiter_.uncertaintyUs(now) + arbiter_.pendingCorrectionUs(now);
  st.last_sync_utc_us = last_sync_utc_;
  st.source = last_source_;

  const int64_t utc = arbiter_.utcAt(now);
  if (!handleSntpRequest(req, len, utc, utc, st, resp48)) return false;

  clients_.touch(client_id, now);
  return true;
}

}  // namespace airtime
