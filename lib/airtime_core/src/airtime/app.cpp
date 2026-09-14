#include "app.h"

namespace airtime {

// The reader's bins must be the sampler's sub blocks, which AppConfig owns.
static SubcarrierReaderConfig readerFor(const AppConfig& cfg) {
  SubcarrierReaderConfig r = cfg.subcarrier_reader;
  r.block_us = cfg.wwv_sub_block_us;
  return r;
}

AirTimeApp::AirTimeApp(const AppDeps& deps, const AppConfig& cfg)
    : deps_(deps),
      cfg_(cfg),
      arbiter_(cfg.arbiter),
      sched_(cfg.scheduler),
      bias_(cfg.station_bias),
      survey_(cfg.survey_cfg),
      marker_(cfg.marker),
      sub_reader_(readerFor(cfg)),
      timecode_(cfg.timecode, cfg.century_hint_year) {}

void AirTimeApp::setFmStations(const int32_t* khz, std::size_t n) {
  if (khz == nullptr) return;
  if (n > kMaxStations) n = kMaxStations;
  for (std::size_t i = 0; i < n; ++i) stations_[i] = khz[i];
  station_count_ = n;
  station_idx_ = 0;
  for (std::size_t i = 0; i < n; ++i) sources_.fm(stations_[i]);
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
    // A station list this radio measured outranks the compile-time seed, which
    // is only a first-boot guess. The firmware supplies that seed BEFORE
    // begin(), so the saved list must replace it rather than wait for an empty
    // slot — waiting is how every survey used to be thrown away at power-on.
    if (deps_.store->loadBlob(kBlobStations, blob, sizeof(blob), &got)) {
      int32_t khz[kMaxStations];
      const std::size_t n = decodeStations(blob, got, khz, kMaxStations);
      if (n > 0) {
        setFmStations(khz, n);
        stations_measured_ = true;
      }
    }
    // Green / Yellow / Red from earlier power-ons. The table verifies the blob
    // whole or ignores it.
    uint8_t sblob[SourceTable::kBlobMax];
    if (deps_.store->loadBlob(kBlobSources, sblob, sizeof(sblob), &got)) {
      sources_.decode(sblob, got);
    }
  }

  // Point both detector channels at their duty before anything can start the
  // sampler: the marker bin and the timecode subcarrier bin. The sampler's own
  // defaults happen to agree today, but the app should not depend on an
  // adapter's initializers matching its config.
  deps_.wwv->setDetector(cfg_.wwv_tone_hz, cfg_.wwv_block_us);
  deps_.wwv->setSubDetector(cfg_.wwv_sub_hz, cfg_.wwv_sub_block_us);

  // Every source the radio will try has a row, and the rotation starts with
  // the ones that have earned it. Red rows get one early retry this power-on.
  for (std::size_t i = 0; i < station_count_; ++i) sources_.fm(stations_[i]);
  for (std::size_t i = 0; i < sched_.bandCount(); ++i) {
    sources_.wwv(sched_.bandStats(i).khz);
  }
  orderStationsByRating();
  for (std::size_t i = 0; i < kMaxStations; ++i) red_tried_[i] = now - cfg_.red_recheck_us;

  sched_.start(now);
  acquire_began_ = now;
  fm_dwell_start_ = now;
  last_persist_ = now;
  last_ct_mono_ = now;   // the list gets a fair run before it is judged
  tuneFmStation(now);

  // Nothing to listen to and nothing remembered: find out for ourselves.
  if (cfg_.auto_survey && station_count_ == 0) startSurvey();

  directive_ = effectiveDirective(sched_.tick(now));
  applyDirective(directive_);
}

void AirTimeApp::startSurvey() {
  survey_.begin(deps_.clock->nowUs());
  survey_started_ = true;
  last_survey_start_ = deps_.clock->nowUs();
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
    ++group_types_[(g.b >> 11) & 0x1F];   // type<<1 | version, for the log
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
  stations_measured_ = true;   // heard here, tonight: this one may be saved
  tuneFmStation(deps_.clock->nowUs());
  learned_dirty_ = true;
  persist(deps_.clock->nowUs(), /*force=*/true);
}

void AirTimeApp::setManualUtc(int64_t utc_us, int64_t uncertainty_us) {
  const int64_t now = deps_.clock->nowUs();
  TimeFix f;
  f.source = Source::Manual;
  f.mono_us = now;
  f.utc_us = utc_us;
  // What the setter is worth: 5 s for an operator with a watch (the default),
  // about 1 s for a phone. Never better than a second, so a manual set cannot
  // outrank a WWV fix in the blend — the point is to hand off to WWV fast.
  f.uncertainty_us = uncertainty_us < 1000000 ? 1000000 : uncertainty_us;
  f.independent_support = 2;   // the operator's own confirmation, see header
  f.carries_date = true;
  const ArbiterUpdate u = arbiter_.update(f);
  if (u.action != Action::Rejected) {
    anchor_ = Anchor::Manual;   // what the clock now stands on: a person's watch
    anchor_pi_ = 0;
    // Reports heard before the set were measured against the clock just thrown
    // away. Voted now, they read as "no error", get accepted, and turn the
    // anchor back to Fm — so the station that was five minutes slow is never
    // checked against the hand-set time, and never turns Red.
    voter_.clear();
    have_new_ct_ = false;
    // The sync age and source mask now describe the hand-set. The status line
    // still reads UNSYNCED: ±5 s is outside the sync threshold, by design.
    noteAccepted(Source::Manual, now);
  }
  ever_synced_ = ever_synced_ || arbiter_.isSynced(now);
  learned_dirty_ = true;
  persist(now, /*force=*/true);
}

void AirTimeApp::tuneRds(int32_t khz) {
  deps_.rds->tuneKhz(khz);
  for (uint32_t& c : group_types_) c = 0;   // counts describe the station on the dial
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

// FM has earned its boot hunt if a station on the list has delivered a clock
// time at this QTH and is not rated Red.
bool AirTimeApp::fmProven() const {
  for (std::size_t i = 0; i < station_count_; ++i) {
    const SourceRow* r = sources_.find(SourceKind::Fm, stations_[i]);
    if (r == nullptr || r->heard == 0) continue;
    const Rating rt = sources_.rating(*r);
    if (rt == Rating::Green || rt == Rating::Yellow) return true;
  }
  return false;
}

int64_t AirTimeApp::hfChangeInUs() const {
  const int64_t now = deps_.clock->nowUs();
  int64_t us = sched_.usUntilPhaseChange(now);
  if (quick_listen_at_ != 0 && sched_.phase() == Phase::Serving) {
    const int64_t q = quick_listen_at_ > now ? quick_listen_at_ - now : 0;
    if (q < us) us = q;
  }
  return us;
}

void AirTimeApp::resetTimecodeChain() {
  sub_reader_.reset();
  timecode_.reset();
  tc_last_edge_us_ = 0;
}

void AirTimeApp::loop() {
  const int64_t now = deps_.clock->nowUs();

  // The scheduler paces itself by whether the time is CONFIRMED, not merely
  // set. One FM station, or a phone, gives a time nothing has checked; the
  // hourly three-minute rhythm then left the owner's radio Yellow all day,
  // with WWV barely tried. Until a second source agrees, the listen windows
  // are still the acquisition, and NTP flags the time unusable meanwhile, so
  // the windows cost nothing.
  sched_.setSeeded(arbiter_.hasSourceFix() && anchor_ == Anchor::Multi);

  // FM that has never delivered a clock time here does not get the whole
  // five-minute boot hunt: two dwells, then HF. The owner's radio sat on FM for
  // five minutes and then waited fifteen more for its first WWV window, at a
  // QTH where FM had already proven useless.
  const bool was_acquiring = sched_.phase() == Phase::Acquiring;
  if (was_acquiring && !arbiter_.hasSourceFix() && !surveying() && !fmProven() &&
      (now - acquire_began_) >= cfg_.unproven_fm_acquire_us) {
    sched_.requestServeNow();
  }
  directive_ = effectiveDirective(sched_.tick(now));
  // Acquisition ended without a CONFIRMED time. With nothing at all, go and
  // listen now. With one FM station's word, give the other stations one short
  // turn to agree — the tuner is single, and a window would take it from them
  // for eight minutes — then check HF instead of waiting a whole unseeded
  // interval. The owner stood at the big antenna for eight minutes hearing FM,
  // because 106.1 had set the clock and the first WWV window was fifteen
  // minutes off. NTP flags an unconfirmed time unusable meanwhile.
  if (was_acquiring && sched_.phase() == Phase::Serving && !surveying() &&
      mode_ == OpMode::Clock) {
    if (!arbiter_.hasSourceFix()) {
      sched_.requestListenNow();
    } else if (anchor_ != Anchor::Multi && cfg_.unconfirmed_listen_after_us > 0) {
      quick_listen_at_ = now + cfg_.unconfirmed_listen_after_us;
    }
  }
  if (quick_listen_at_ != 0) {
    if (anchor_ == Anchor::Multi || mode_ != OpMode::Clock) {
      quick_listen_at_ = 0;       // FM confirmed it, or the operator has the dial
    } else if (now >= quick_listen_at_ && sched_.phase() == Phase::Serving &&
               !surveying()) {
      sched_.requestListenNow();
      quick_listen_at_ = 0;
    }
  }
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
    ++group_types_[(g.b >> 11) & 0x1F];   // type<<1 | version, for the log
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

    // Rate the station that spoke. Its error is taken after its learned bias,
    // against the clock as it stood at reception.
    SourceRow* row = sources_.fm(deps_.rds->tunedKhz());
    if (row != nullptr) row->pi = g.a;
    noteFmSourceTime(row, g.a, asserted + bias_.correction(g.a) - reference, now);
    dwell_heard_ct_ = true;
    // A station proven wrong against a confirmed clock does not vote. It is
    // still heard, and the first good time it sends puts it back.
    if (row != nullptr && sources_.rating(*row) == Rating::Red) continue;

    CtReport r;
    r.pi = g.a;
    // Put the station back on time before it votes. Zero until the station has
    // been measured enough times to be trusted.
    r.asserted_utc_us = asserted + bias_.correction(g.a);
    r.reference_us = reference;
    r.rx_monotonic_us = g.mono_us;
    voter_.add(r);
    have_new_ct_ = true;
    last_ct_mono_ = g.mono_us;
  }

  // Rotate the FM scan so every receivable station gets a chance to report —
  // but never while a WWV listen window owns the tuner. The fakes are two
  // independent radios; the device has ONE, and an ungated rotation here
  // yanked the chip from AM back to FM 75 s into every real listen window.
  if (!directive_.wwv_listening && station_count_ > 0 &&
      (now - fm_dwell_start_) >= cfg_.fm_dwell_us) {
    // The dwell is over: the station either delivered clock time or it did not.
    sources_.noteDwellEnd(sources_.fm(stations_[station_idx_]), dwell_heard_ct_);
    dwell_heard_ct_ = false;
    learned_dirty_ = true;
    if (station_count_ > 1) {
      // Next station, passing over Red ones — each still gets one retry per
      // recheck interval, because a silent or wrong station can come back.
      std::size_t next = station_idx_;
      for (std::size_t k = 0; k < station_count_; ++k) {
        next = (next + 1) % station_count_;
        const SourceRow* sr = sources_.find(SourceKind::Fm, stations_[next]);
        if (sr == nullptr || sources_.rating(*sr) != Rating::Red) break;
        if ((now - red_tried_[next]) >= cfg_.red_recheck_us) {
          red_tried_[next] = now;
          break;
        }
      }
      station_idx_ = next;
      tuneFmStation(now);
    } else {
      fm_dwell_start_ = now;
    }
  }

  // A list that never delivers is worth nothing, however it was chosen. Go and
  // find stations that do: the survey keeps what it hears, and the list it
  // adopts now survives a reboot. Never inside a listen window — that tuner
  // belongs to WWV.
  if (cfg_.auto_survey && !directive_.wwv_listening &&
      (now - last_ct_mono_) >= cfg_.silent_list_survey_after_us &&
      (!survey_started_ || (now - last_survey_start_) >= cfg_.survey_retry_us)) {
    startSurvey();
    return;
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
  const VoteResult vr =
      voter_.vote(cfg_.rds_vote_tolerance_us, arbiter_.hasSourceFix());
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

  if (u.action != Action::Rejected) {
    noteAccepted(Source::Rds, now);
    if (vr.agreeing_stations >= 2) {
      anchor_ = Anchor::Multi;             // stations confirming each other
    } else if (anchor_ == Anchor::None || anchor_ == Anchor::Manual) {
      anchor_ = Anchor::Fm;                // one station's word, unconfirmed
      anchor_pi_ = vr.center_pi;
    }
  }
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
      noteWwvSourceTime(off);
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
  while (deps_.wwv->nextSubPower(&t, &p)) {
    const uint32_t locks_before = sub_reader_.diag().locks;
    sub_reader_.process(t, p);
    if (sub_reader_.diag().locks != locks_before) {
      // A new second boundary. Whatever frame alignment the decoder held was
      // measured against the old one.
      timecode_.reset();
      // The code's shape standing this far out of the noise is propagation
      // evidence in itself: hold the band while the frames arrive.
      sched_.onWwvMarker(sub_reader_.diag().on_level);
    }
  }

  // One entry per second from the moment the reader locked: a width, or -1
  // for a second it could not read. Holes keep the frame's shape (decoder
  // header, "Holes"), so every second is fed, readable or not.
  int64_t pulse_ms = 0;
  int64_t edge_us = 0;
  while (sub_reader_.next(&pulse_ms, &edge_us)) {
    if (pulse_ms < 0) {
      ++tc_diag_.gap_seconds;
    } else {
      ++tc_diag_.pulses;
    }
    tc_last_edge_us_ = edge_us;

    const bool confirmed = timecode_.onSecond(pulse_ms, edge_us);

    // A frame that DECODES is propagation evidence as strong as a marker —
    // noise does not produce sixty structurally-correct symbols — so it pins
    // the band rotation and credits the band table even before its partner
    // frame confirms the time.
    if (timecode_.framesDecoded() != tc_frames_decoded_seen_) {
      tc_frames_decoded_seen_ = timecode_.framesDecoded();
      sched_.onWwvMarker(sub_reader_.diag().on_level);
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
      noteWwvSourceTime(u.offset_us);
      sched_.onWwvFix(now);
    }
  }
}

void AirTimeApp::noteFmSourceTime(SourceRow* row, uint16_t pi, int64_t err_us,
                                  int64_t now) {
  if (row == nullptr) return;
  learned_dirty_ = true;
  // A hand-set clock cannot confirm a station, but it can catch one that is
  // minutes out: 92.3 ran five minutes slow on the owner's radio, and was the
  // only clock-time station on the dial, so nothing else was there to say so.
  if (anchor_ == Anchor::Manual && arbiter_.isSet() &&
      (err_us > cfg_.manual_wrong_us || err_us < -cfg_.manual_wrong_us)) {
    sources_.noteWrong(row, err_us);
    return;
  }
  // Only a clock backed by a real, synced, DIFFERENT source can check this one.
  const bool different = anchor_ == Anchor::Multi || anchor_ == Anchor::Wwv ||
                         (anchor_ == Anchor::Fm && pi != anchor_pi_);
  const bool confirmable =
      different && arbiter_.hasSourceFix() && arbiter_.isSynced(now);
  const int64_t mag = err_us < 0 ? -err_us : err_us;
  // One unconfirmed station cannot convict another. Two single stations that
  // disagree say one of them is wrong, not which: on the owner's radio 106.1,
  // about 3 s slow, set the clock and then marked 98.9 Red for being right.
  // Agreement still confirms both; disagreement waits for a third source.
  if (confirmable && anchor_ == Anchor::Fm && mag > cfg_.rds_vote_tolerance_us) {
    sources_.noteTime(row, err_us, false);
    return;
  }
  sources_.noteTime(row, err_us, confirmable);
  if (!confirmable || mag > cfg_.rds_vote_tolerance_us) return;
  // Agreement runs both ways: the source the clock stands on is confirmed too.
  if (anchor_ == Anchor::Fm) sources_.noteConfirmed(sources_.findPi(anchor_pi_));
  if (anchor_ == Anchor::Wwv) sources_.noteConfirmed(sources_.wwv(anchor_wwv_khz_));
  anchor_ = Anchor::Multi;
}

void AirTimeApp::noteWwvSourceTime(int64_t err_us) {
  const int32_t khz = tuned_wwv_khz_ != 0 ? tuned_wwv_khz_ : sched_.currentBandKhz();
  SourceRow* row = sources_.wwv(khz);
  if (row == nullptr) return;
  learned_dirty_ = true;
  // WWV is one station on every band: only FM can confirm it.
  const bool different = anchor_ == Anchor::Fm || anchor_ == Anchor::Multi;
  const int64_t mag = err_us < 0 ? -err_us : err_us;
  if (different && mag <= cfg_.rds_vote_tolerance_us) {
    sources_.noteTime(row, err_us, true);
    if (anchor_ == Anchor::Fm) sources_.noteConfirmed(sources_.findPi(anchor_pi_));
    anchor_ = Anchor::Multi;
    return;
  }
  // Only ACCEPTED fixes arrive here, so where this one disagreed with the old
  // anchor the clock now stands on WWV: its markers seconded each other, or it
  // carried the date. It is heard, not judged against the source it just
  // overruled — that would mark WWV Red for being right whenever the station
  // that set the clock was the wrong one. The overruled source is judged by
  // its own next report, against a clock that is now WWV's.
  sources_.noteTime(row, err_us, false);
  anchor_ = Anchor::Wwv;
  anchor_wwv_khz_ = khz;
}

void AirTimeApp::orderStationsByRating() {
  const auto rank = [this](int32_t khz) {
    const SourceRow* r = sources_.find(SourceKind::Fm, khz);
    switch (r != nullptr ? sources_.rating(*r) : Rating::Unknown) {
      case Rating::Green: return 0;
      case Rating::Yellow: return 1;
      case Rating::Unknown: return 2;
      default: return 3;
    }
  };
  for (std::size_t a = 1; a < station_count_; ++a) {   // stable insertion sort
    const int32_t key = stations_[a];
    std::size_t b = a;
    while (b > 0 && rank(stations_[b - 1]) > rank(key)) {
      stations_[b] = stations_[b - 1];
      --b;
    }
    stations_[b] = key;
  }
  station_idx_ = 0;
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
    // Only a measured list. The compiled seed is a guess, and saving it would
    // make the guess outrank the next firmware's better one at every boot.
    if (stations_measured_) {
      n = encodeStations(stations_, station_count_, blob, sizeof(blob));
      if (n > 0) deps_.store->saveBlob(kBlobStations, blob, n);
    }
    uint8_t sblob[SourceTable::kBlobMax];
    n = sources_.encode(sblob, sizeof(sblob));
    if (n > 0) deps_.store->saveBlob(kBlobSources, sblob, n);
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
  st.confirmed = anchor_ == Anchor::Multi;
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
  // A laptop must not set itself from time nothing has confirmed: this radio
  // once served a station's clock five minutes slow as stratum 1. Until a
  // DIFFERENT source agrees, the answer carries the alarm flag, and clients
  // that respect it leave their clocks alone.
  st.synced = arbiter_.isSynced(now) && anchor_ == Anchor::Multi;
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
