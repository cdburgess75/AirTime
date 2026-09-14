#include "scheduler.h"

namespace airtime {

Scheduler::Scheduler(const SchedulerConfig& cfg) : cfg_(cfg) {
  static const int32_t kDefaultBands[] = {5000, 10000, 15000};  // §4 rotation
  setBands(kDefaultBands, 3);
}

void Scheduler::setBands(const int32_t* khz, std::size_t n) {
  if (khz == nullptr || n == 0) return;
  if (n > kMaxBands) n = kMaxBands;
  for (std::size_t i = 0; i < n; ++i) {
    bands_[i] = BandStats{};
    bands_[i].khz = khz[i];
  }
  band_count_ = n;
  band_idx_ = 0;
}

void Scheduler::start(int64_t mono_us) {
  started_ = true;
  phase_ = Phase::Acquiring;   // §5: every power-on begins with WiFi DOWN
  acquire_start_ = mono_us;
  band_start_ = mono_us;
  band_idx_ = 0;
  has_fix_ = false;
  want_listen_ = false;
  want_serve_ = false;
  band_productive_ = false;
  if (band_count_ > 0) bands_[band_idx_].attempts++;
}

// Rotate to the next band once the dwell expires — but ONLY off a band that
// has gone silent. A band that is delivering markers is the one we came for,
// and leaving it costs more than the dwell was ever meant to buy:
//
//   * a phase fix beyond the step threshold needs TWO markers a minute apart
//     (app.cpp, WWV self-corroboration), so a step mid-window destroys the
//     pair and the correction can never be applied; and
//   * retuning resets the marker detector — a tone in flight when the step
//     lands is lost outright, its leading edge never timestamped.
//
// Observed in simulation with the fixed 2-minute dwell inside a 3-minute
// window: the step landed on the second marker of every window, forever. The
// dwell now bounds patience with a DEAD band, which is what §4 meant by it.
void Scheduler::maybeStepBand(int64_t mono_us) {
  if (band_count_ == 0 || band_productive_) return;
  if ((mono_us - band_start_) < bandDwellUs()) return;
  band_idx_ = (band_idx_ + 1) % band_count_;
  band_start_ = mono_us;
  band_productive_ = false;
  bands_[band_idx_].attempts++;
}

int64_t Scheduler::usUntilPhaseChange(int64_t mono_us) const {
  int64_t due = mono_us;
  switch (phase_) {
    case Phase::Acquiring: due = acquire_start_ + cfg_.acquire_timeout_us; break;
    case Phase::Serving:   due = last_listen_end_ + listenIntervalUs(); break;
    case Phase::Listening: due = listen_start_ + listenDurationUs(); break;
  }
  return due > mono_us ? due - mono_us : 0;
}

int32_t Scheduler::currentBandKhz() const {
  return band_count_ > 0 ? bands_[band_idx_].khz : 0;
}

std::size_t Scheduler::preferredBandIndex() const {
  std::size_t best = band_idx_;
  int best_n = -1;
  for (std::size_t i = 0; i < band_count_; ++i) {
    if (creditFor(i) > best_n) {
      best_n = creditFor(i);
      best = i;
    }
  }
  return best;
}

int Scheduler::creditFor(std::size_t i) const {
  return utc_block_ >= 0 ? static_cast<int>(bands_[i].by_block[utc_block_])
                         : bands_[i].successes;
}

Directive Scheduler::directive() const {
  Directive d;
  d.phase = phase_;
  switch (phase_) {
    case Phase::Acquiring:  // parallel hunt, WiFi down
      d.wifi_up = false;
      d.wwv_listening = true;
      d.rds_scanning = true;
      break;
    case Phase::Serving:  // NTP up; RDS keeps running (no WiFi conflict)
      d.wifi_up = true;
      d.wwv_listening = false;
      d.rds_scanning = true;
      break;
    case Phase::Listening:  // WiFi torn down for the window
      d.wifi_up = false;
      d.wwv_listening = true;
      d.rds_scanning = true;
      break;
  }
  d.wwv_band_khz = d.wwv_listening ? currentBandKhz() : 0;
  return d;
}

void Scheduler::enterServing(int64_t mono_us) {
  if (phase_ == Phase::Listening) last_window_productive_ = band_productive_;
  // A window that heard nothing hands the next one a fresh band. Without this
  // the sweep cannot finish: a 3-minute window holds at most two 2-minute
  // dwells, and every window restarted at the same place, so the third band was
  // never reached at all. Measured at the owner's QTH — three consecutive
  // windows tried 5 MHz then 10 MHz and stopped, while the only band that has
  // ever produced a marker there is 15 MHz.
  if (phase_ == Phase::Listening && !band_productive_ && band_count_ > 0) {
    band_idx_ = (band_idx_ + 1) % band_count_;
  }
  phase_ = Phase::Serving;
  last_listen_end_ = mono_us;
  want_listen_ = false;
  want_serve_ = false;
}

void Scheduler::enterListening(int64_t mono_us) {
  phase_ = Phase::Listening;
  listen_start_ = mono_us;
  band_start_ = mono_us;
  // Start the window on the band that has been working locally, then rotate on
  // through the others as the window runs.
  // Open on a band that has actually delivered. Until one has, leave the sweep
  // cursor alone — enterServing advanced it past whatever was silent last time,
  // and overriding that here is what made the rotation loop over the same two
  // bands forever.
  //
  // Two exceptions keep one early success from pinning the radio to one band
  // for good (it sat on 10 MHz all afternoon): after a window that heard
  // nothing, the cursor enterServing moved past that band stays put; and every
  // explore_every-th window samples the next band on purpose.
  ++windows_opened_;
  const std::size_t pref = preferredBandIndex();
  const bool earned = band_count_ > 0 && creditFor(pref) > 0;
  const bool sample = cfg_.explore_every > 0 && band_count_ > 1 &&
                      windows_opened_ % static_cast<uint32_t>(cfg_.explore_every) == 0;
  if (earned && sample) {
    band_idx_ = (pref + 1) % band_count_;
  } else if (earned && last_window_productive_) {
    band_idx_ = pref;
  }
  if (band_count_ > 0) bands_[band_idx_].attempts++;
  want_listen_ = false;
  want_serve_ = false;
  // Each window re-earns its band: propagation at 03:00 says little about
  // propagation at noon, and the preferred band must be able to lose.
  band_productive_ = false;
}

Directive Scheduler::tick(int64_t mono_us) {
  if (!started_) start(mono_us);

  switch (phase_) {
    case Phase::Acquiring: {
      // An operator's Listen Now is honoured here too. It used to wait for
      // the hunt to end and then be wiped by enterServing(): the owner pressed
      // it at power-on and heard FM for another fifteen minutes.
      if (want_listen_) {
        enterServing(mono_us);
        enterListening(mono_us);
        break;
      }
      // §5: serve on the first credible fix, or when the hunt times out.
      if (has_fix_ || want_serve_ ||
          (mono_us - acquire_start_) >= cfg_.acquire_timeout_us) {
        enterServing(mono_us);
        break;
      }
      maybeStepBand(mono_us);
      break;
    }

    case Phase::Serving: {
      if (want_listen_ ||
          (mono_us - last_listen_end_) >= listenIntervalUs()) {
        enterListening(mono_us);
      }
      break;
    }

    case Phase::Listening: {
      if (want_serve_ ||
          (mono_us - listen_start_) >= listenDurationUs()) {
        enterServing(mono_us);
        break;
      }
      maybeStepBand(mono_us);
      break;
    }
  }

  return directive();
}

void Scheduler::onWwvMarker(real snr) {
  band_productive_ = true;  // hold this band; see maybeStepBand
  if (band_count_ > 0) {
    BandStats& b = bands_[band_idx_];
    b.successes++;
    if (utc_block_ >= 0 && b.by_block[utc_block_] < 0xFFFF) b.by_block[utc_block_]++;
    if (snr > b.best_snr) b.best_snr = snr;
  }
}

void Scheduler::onWwvFix(int64_t mono_us) {
  has_fix_ = true;
  if (phase_ == Phase::Listening && cfg_.exit_listen_on_fix) {
    enterServing(mono_us);
  }
}

bool Scheduler::seedBandStats(int32_t khz, int successes, real best_snr,
                              const uint16_t* by_block) {
  for (std::size_t i = 0; i < band_count_; ++i) {
    if (bands_[i].khz != khz) continue;
    bands_[i].successes = successes;
    if (by_block != nullptr) {
      for (int k = 0; k < 4; ++k) bands_[i].by_block[k] = by_block[k];
    }
    if (best_snr > bands_[i].best_snr) bands_[i].best_snr = best_snr;
    return true;
  }
  return false;
}

void Scheduler::onRdsFix(int64_t mono_us) {
  (void)mono_us;
  has_fix_ = true;  // acted on at the next tick (§5: serve on first fix)
}

void Scheduler::requestListenNow() { want_listen_ = true; }
void Scheduler::requestServeNow() { want_serve_ = true; }

}  // namespace airtime
