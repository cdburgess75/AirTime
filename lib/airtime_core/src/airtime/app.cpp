#include "app.h"

namespace airtime {

AirTimeApp::AirTimeApp(const AppDeps& deps, const AppConfig& cfg)
    : deps_(deps),
      cfg_(cfg),
      arbiter_(cfg.arbiter),
      sched_(cfg.scheduler),
      bias_(cfg.station_bias),
      survey_(cfg.survey_cfg),
      marker_(cfg.marker),
      pulse_(cfg.subcarrier_pulse),
      timecode_(cfg.timecode, cfg.century_hint_year) {}

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
    // A surveyed station list outranks the compile-time warm start: it was
    // measured HERE. Only adopted if the caller did not supply one explicitly.
    if (station_count_ == 0 &&
        deps_.store->loadBlob(kBlobStations, blob, sizeof(blob), &got)) {
      int32_t khz[kMaxStations];
      const std::size_t n = decodeStations(blob, got, khz, kMaxStations);
      if (n > 0) setFmStations(khz, n);
    }
  }

  // Point both detector channels at their duty before anything can start the
  // sampler: the marker bin and the timecode subcarrier bin. The sampler's own
  // defaults happen to agree today, but the app should not depend on an
  // adapter's initializers matching its config.
  deps_.wwv->setDetector(cfg_.wwv_tone_hz, cfg_.wwv_block_us);
  deps_.wwv->setSubDetector(cfg_.wwv_sub_hz, cfg_.wwv_sub_block_us);

  sched_.start(now);
  fm_dwell_start_ = now;
  last_persist_ = now;
  tuneFmStation(now);

  // Nothing to listen to and nothing remembered: find out for ourselves.
  if (cfg_.auto_survey && station_count_ == 0) startSurvey();

  directive_ = effectiveDirective(sched_.tick(now));
  applyDirective(directive_);
}

void AirTimeApp::startSurvey() {
  survey_.begin(deps_.clock->nowUs());
  const int32_t khz = survey_.wantTuned();
  if (khz != 0) tuneRds(khz);
}

void AirTimeApp::pollSurvey(int64_t now) {
  // Listen while dwelling — without this the survey would tune beautifully and
  // hear nothing, and every station would score as "no clock time".
  //
  // The disagreement is measured against our own clock, which during a survey
  // may be wildly wrong or unset; that is fine and deliberate. FmSurvey
  // re-references everything to its own median at the end, so only a STABLE
  // clock is required, not a correct one.
  RdsGroup g;
  while (deps_.rds->poll(&g)) {
    RdsClockTime t;
    if (!decodeRdsClockTime(g.a, g.b, g.c, g.d, &t)) continue;
    const int64_t asserted = t.utc_epoch_s * 1000000;
    const int64_t reference =
        arbiter_.isSet() ? arbiter_.utcAt(g.mono_us) : g.mono_us;
    survey_.noteClockTime(g.a, reference - asserted);
  }

  const int32_t before = survey_.wantTuned();
  if (survey_.tick(now, deps_.rds->signalStrength())) {
    const int32_t khz = survey_.wantTuned();
    if (khz != 0 && khz != before) tuneRds(khz);
  }
  if (survey_.done()) adoptSurveyResult();
}

void AirTimeApp::adoptSurveyResult() {
  int32_t found[kMaxStations];
  const std::size_t n = survey_.results(found, kMaxStations);
  survey_.abort();   // back to Idle: the result is taken, the dial is free

  // Nothing usable. Leave whatever list was already there rather than blanking
  // it — a bad night on the dial is not a reason to forget a good station.
  if (n == 0) return;

  setFmStations(found, n);
  tuneFmStation(deps_.clock->nowUs());
  learned_dirty_ = true;
  persist(deps_.clock->nowUs(), /*force=*/true);
}

void AirTimeApp::setManualUtc(int64_t utc_us) {
  const int64_t now = deps_.clock->nowUs();
  TimeFix f;
  f.source = Source::Manual;
  f.mono_us = now;
  f.utc_us = utc_us;
  // What an operator reading a watch and pressing a button is actually worth.
  // Deliberately not optimistic: claiming better than this would let a manual
  // set outrank a WWV marker in the blend, and the whole point is to hand off
  // to WWV as fast as possible.
  f.uncertainty_us = 5000000;
  f.independent_support = 2;   // the operator's own confirmation, see header
  f.carries_date = true;
  arbiter_.update(f);
  ever_synced_ = ever_synced_ || arbiter_.isSynced(now);
  learned_dirty_ = true;
  persist(now, /*force=*/true);
}

void AirTimeApp::tuneRds(int32_t khz) {
  deps_.rds->tuneKhz(khz);
  // One tuner: the dial is on FM now, whatever the WWV cache used to claim.
  // Leaving that claim standing is how a listen window ends up sampling FM
  // program audio — applyDirective skips the WWV retune when the wanted band
  // "already matches" a chip that has long since been moved. The music heard
  // on "10 MHz" in the first field session was in all likelihood this bug:
  // the mkr line printed the wanted band while the chip sat on a local FM
  // station, and the inflated noise floor read as jamming.
  tuned_wwv_khz_ = 0;
}

void AirTimeApp::tuneFmStation(int64_t now) {
  if (station_count_ == 0) return;
  tuneRds(stations_[station_idx_]);
  fm_dwell_start_ = now;
}

void AirTimeApp::setMode(OpMode m) {
  if (m == mode_) return;
  const OpMode prev = mode_;

  // Leaving CW: the character still being assembled comes out — the last
  // letter of a callsign must not be eaten by the mode change — and the shared
  // detector goes back to being a WWV instrument. setDetector() is only legal
  // on a stopped sampler, which is why the stop happens HERE, synchronously,
  // rather than being left to applyDirective a loop later. This ordering used
  // to live in the firmware as three calls whose comment admitted the order
  // was load-bearing; now it lives where the fakes can test it.
  if (prev == OpMode::Cw) {
    char c = 0;
    if (cw_.flush(&c)) cw_text_.push(c);
    if (deps_.wwv->isRunning()) deps_.wwv->stop();
    deps_.wwv->setDetector(cfg_.wwv_tone_hz, cfg_.wwv_block_us);
    deps_.wwv->setSubDetector(cfg_.wwv_sub_hz, cfg_.wwv_sub_block_us);
  }

  if (m == OpMode::Cw) {
    // Entering CW: take the tap. A listen window may own the sampler at this
    // instant — stop it first, same reason as above. Decoder and noise floor
    // start from silence: both are about the band the operator is about to
    // tune, not the one the radio just left. The subcarrier channel goes
    // quiet too: its blocks would be 50 ms of somebody's sidetone, and an
    // unread queue is drops in the diagnostic that WWV listening relies on.
    if (deps_.wwv->isRunning()) deps_.wwv->stop();
    deps_.wwv->setDetector(cfg_.cw_tone_hz, cfg_.cw_block_us);
    deps_.wwv->setSubDetector(0.0f, 0);
    cw_.reset();
    cw_text_.clear();
    cw_level_ = 0.0f;
  }

  // Leaving the waterfall: release the tap NOW, not a loop later, so the
  // firmware can safely tear the spectrum bank down the moment this returns.
  if (prev == OpMode::Spectrum) {
    if (deps_.wwv->isRunning()) deps_.wwv->stop();
  }

  if (m == OpMode::Spectrum) {
    // Entering the waterfall: take the tap, stopped first for the same
    // reason. The firmware layer flips the sampler into spectrum duty between
    // this stop and the restart applyDirective performs next loop — the gap
    // is the point, it is what makes the reconfiguration race-free.
    if (deps_.wwv->isRunning()) deps_.wwv->stop();
  }

  mode_ = m;

  // Returning to clock duty. For however long the operator had the dial, every
  // cached belief about it went stale — selectBand() goes through neither
  // adapter. Retune the FM side now, not at the next dwell rotation (which
  // with one station never comes), and tuneRds() voids the WWV claim, forcing
  // a genuine retune into the next listen window.
  if (m == OpMode::Clock) tuneFmStation(deps_.clock->nowUs());
}

real AirTimeApp::cwSnr() const {
  const real floor = cw_.noiseFloor();
  if (floor <= 0.0f || cw_level_ <= 0.0f) return 0.0f;
  return cw_level_ / floor;
}

void AirTimeApp::pollCw(int64_t) {
  int64_t t = 0;
  real p = 0.0f;
  char c = 0;
  while (deps_.wwv->nextPower(&t, &p)) {
    cw_level_ = p;
    if (cw_.process(t, p, &c)) cw_text_.push(c);
  }
}

Directive AirTimeApp::effectiveDirective(Directive d) const {
  // CW first: it is the stricter of the two operator modes. Same audio tap as
  // WWV, therefore the same silicon rule — WiFi OFF or ADC2 reads garbage
  // (PLAN.md §2). The access point goes down and NTP stops answering for as
  // long as the operator stays here, which is why this mode is never entered
  // by the scheduler and only ever by a person.
  //
  // wwv_listening stays FALSE: the sampler runs, but these blocks are 5 ms of
  // a ~700 Hz beat note, not 20 ms of a 1000 Hz minute marker. Feeding them to
  // the marker detector would hand the arbiter phase measurements derived from
  // somebody's callsign.
  if (mode_ == OpMode::Cw || mode_ == OpMode::Spectrum) {
    d.wwv_listening = false;
    d.wwv_band_khz = 0;
    d.rds_scanning = false;
    d.wifi_up = false;
    return d;
  }

  // Operator mode: the dial belongs to the human. Nothing here may retune, so
  // the scheduler's listening plans are simply overruled — and with no ADC
  // sampling there is no reason for WiFi to drop, so NTP serves continuously
  // instead of coasting through a window every hour.
  if (mode_ == OpMode::Radio) {
    d.wwv_listening = false;
    d.wwv_band_khz = 0;
    d.rds_scanning = false;
    d.wifi_up = true;
    return d;
  }

  // Unseeded WWV listening was once suppressed everywhere: a ±30 s marker
  // cannot start a clock, and on the single-tuner radio a pre-seed listen
  // window starves the RDS path that could. The 100 Hz timecode changed half
  // of that — it carries the date, so a listen window CAN now start the clock
  // from HF alone — but the tuner arithmetic still stands during the boot
  // hunt, where RDS seeds in ~a minute when it works at all and deserves the
  // first uninterrupted claim on the dial. So the suppression now applies to
  // the ACQUIRING phase only. Once acquisition times out into Serving, the
  // scheduler's unseeded cadence opens real windows and the timecode chain
  // hunts; the marker path stays gated inside pollWwv regardless, because a
  // dateless marker unseeded is exactly as meaningless as it ever was.
  //
  // The test is "has a real source spoken yet", NOT "is the clock set". A warm
  // boot sets the clock from NVS, and that memory is only as good as the
  // power-down was short — measured on the device: an hour stale.
  if (!arbiter_.hasSourceFix() && d.wwv_listening && d.phase == Phase::Acquiring) {
    d.wwv_listening = false;
    d.wwv_band_khz = 0;
  }
  return d;
}

void AirTimeApp::applyDirective(const Directive& d) {
  // THE ordering rule (PLAN.md §2): ADC2 and WiFi can never be live together.
  // Always release before acquiring — stop the sampler and drop WiFi first, then
  // bring up whatever the new directive wants.
  const bool want_audio = d.wwv_listening || mode_ == OpMode::Cw ||
                          mode_ == OpMode::Spectrum;

  if (!want_audio && deps_.wwv->isRunning()) {
    deps_.wwv->stop();
    // The window is over and the symbol cadence with it. A partial frame held
    // across an hour of serving would be continued with next window's seconds
    // — sixty stale symbols would have to fail their structure checks before
    // the hunt could even begin, a minute of the new window spent disproving
    // the old one. Continuity is broken; say so.
    resetTimecodeChain();
    // ONE tuner (PLAN.md §2's quieter sibling): the listen window leaves the
    // chip parked on an AM band, and nothing else puts it back. The FM dwell
    // rotation cannot — it requires station_count_ > 1, so a single-station
    // config would stay RDS-deaf FOREVER after its first window, coasting on
    // drift while the logs showed a healthy station list. Found the day the
    // test fakes learned to share one tuner; invisible while they were two
    // independent radios, and invisible on the bench because the real config
    // happens to carry three stations and the rotation papered over it.
    //
    // Not in radio/CW mode (the dial is the operator's — but then want_audio
    // or the mode gate keeps us out of here anyway) and not mid-survey (the
    // survey owns the tuning plan).
    if (mode_ == OpMode::Clock && !surveying()) {
      tuneFmStation(deps_.clock->nowUs());
    }
  }
  if (!d.wifi_up && deps_.wifi->isUp()) deps_.wifi->tearDown();

  if (d.wifi_up && !deps_.wifi->isUp()) deps_.wifi->bringUp();

  // CW takes the audio tap without touching the dial: the operator tuned it,
  // by ear, and is probably still nudging it.
  if ((mode_ == OpMode::Cw || mode_ == OpMode::Spectrum) &&
      !deps_.wwv->isRunning())
    deps_.wwv->start();

  if (d.wwv_listening) {
    if (d.wwv_band_khz != tuned_wwv_khz_) {
      deps_.wwv->tuneKhz(d.wwv_band_khz);
      tuned_wwv_khz_ = d.wwv_band_khz;
      marker_.reset();  // new band, new noise floor
      resetTimecodeChain();  // and a new station's frame alignment
    }
    if (!deps_.wwv->isRunning()) deps_.wwv->start();
  }
}

void AirTimeApp::resetTimecodeChain() {
  pulse_.reset();
  timecode_.reset();
  tc_last_edge_us_ = 0;
}

void AirTimeApp::loop() {
  const int64_t now = deps_.clock->nowUs();

  // The scheduler paces itself by whether anything has fixed the clock yet:
  // unseeded, the listen windows are the acquisition and come accordingly.
  sched_.setSeeded(arbiter_.hasSourceFix());
  directive_ = effectiveDirective(sched_.tick(now));
  applyDirective(directive_);

  // In operator mode AirTime observes nothing and steers nothing. Reading RDS
  // from whatever the operator happens to tune would let one unvetted station
  // steer the clock, which is exactly the failure the voter exists to prevent.
  if (mode_ == OpMode::Cw) {
    pollCw(now);
    persist(now, /*force=*/false);
    return;
  }

  if (mode_ == OpMode::Radio || mode_ == OpMode::Spectrum) {
    persist(now, /*force=*/false);
    return;
  }

  if (surveying()) {
    pollSurvey(now);
  } else {
    pollRds(now);
    if (directive_.wwv_listening) pollWwv(now);
  }

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
    tuneFmStation(now);
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
  f.carries_date = true;   // CT groups carry the full civil date

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
    f.carries_date = false;  // a marker asserts a boundary, never a minute

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

  pollWwvTimecode(now);
}

// The 100 Hz subcarrier stream: burst widths become symbols, symbols become
// frames, and a confirmed frame is the one WWV product that carries the DATE
// — the fix that can start this clock from HF alone (PLAN.md §3 called that
// out of scope for v1, and the QTH's marginal FM dial is why it no longer is).
void AirTimeApp::pollWwvTimecode(int64_t now) {
  int64_t t = 0;
  real p = 0;
  WwvMarker pm;
  while (deps_.wwv->nextSubPower(&t, &p)) {
    if (!pulse_.process(t, p, &pm)) continue;

    if (tc_last_edge_us_ != 0) {
      const int64_t dt = pm.leading_edge_us - tc_last_edge_us_;
      if (dt < 700000) {
        // A second burst inside the same second. The code sends exactly one
        // pulse per second and the longest legal pulse ends 860 ms in, so
        // this is voice or noise split by the hysteresis — drop the splinter,
        // keep the cadence anchored on the real edge.
        ++tc_diag_.splinters;
        continue;
      }
      if (dt > 90000000) {
        // The band went quiet for a minute and a half. Whatever alignment we
        // held describes a signal that is gone; hunt fresh rather than feed
        // ninety synthetic holes.
        timecode_.reset();
        tc_last_edge_us_ = 0;
      } else {
        // Seconds whose pulse never crossed the threshold. Feed each as an
        // explicitly unreadable symbol so the frame keeps its shape: a holed
        // frame is discarded either way, but an intact marker skeleton keeps
        // the ALIGNMENT, and that is a minute of re-hunting saved (decoder
        // header, "Holes").
        const int64_t missed = (dt - 500000) / 1000000;
        for (int64_t k = 1; k <= missed; ++k) {
          ++tc_diag_.gap_seconds;
          timecode_.onSecond(-1, tc_last_edge_us_ + k * 1000000);
        }
      }
    }
    tc_last_edge_us_ = pm.leading_edge_us;
    ++tc_diag_.pulses;

    const bool confirmed =
        timecode_.onSecond(pm.duration_us / 1000, pm.leading_edge_us);

    // A frame that DECODES is propagation evidence as strong as a marker —
    // noise does not produce sixty structurally-correct symbols — so it pins
    // the band rotation and credits the band table even before its partner
    // frame confirms the time.
    if (timecode_.framesDecoded() != tc_frames_decoded_seen_) {
      tc_frames_decoded_seen_ = timecode_.framesDecoded();
      sched_.onWwvMarker(pm.peak_power);
      learned_dirty_ = true;
    }

    if (!confirmed) continue;

    // Two frames in exact whole-minute lockstep. utc at the frame's second-0
    // boundary, corrected for where the measured edge actually sits: the code
    // pulse opens wwv_timecode_lead_us after its second, and the receive
    // chain delays it wwv_calibration_us more.
    TimeFix f;
    f.source = Source::Wwv;
    f.mono_us = timecode_.frameStartUs();
    f.utc_us = wwvTimeToEpochS(timecode_.time()) * 1000000 +
               cfg_.wwv_timecode_lead_us + cfg_.wwv_calibration_us;
    f.uncertainty_us = cfg_.wwv_timecode_uncertainty_us;
    // The same §4 rule-3 argument as the marker pair (AppConfig::wwv_pair_*):
    // two independent transmissions, a minute or more apart, agreeing through
    // sixty structural checks each AND exact epoch arithmetic. Random audio
    // does not do that twice running.
    f.independent_support = 2;
    f.carries_date = true;   // the entire point of this chain

    const ArbiterUpdate u = arbiter_.update(f);
    tc_diag_.have = true;
    tc_diag_.offset_us = u.offset_us;
    tc_diag_.accepted = u.action != Action::Rejected;

    if (u.action != Action::Rejected) {
      have_prev_wwv_ = false;   // the clock moved; the held marker is stale
      have_wwv_accept_ = true;
      last_wwv_accept_mono_ = f.mono_us;
      noteAccepted(Source::Wwv, now);
      sched_.onWwvFix(now);
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
  f.carries_date = true;
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
    n = encodeStations(stations_, station_count_, blob, sizeof(blob));
    if (n > 0) deps_.store->saveBlob(kBlobStations, blob, n);
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
