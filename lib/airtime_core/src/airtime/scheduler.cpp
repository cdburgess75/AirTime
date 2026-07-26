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
  if ((mono_us - band_start_) < cfg_.band_dwell_us) return;
  band_idx_ = (band_idx_ + 1) % band_count_;
  band_start_ = mono_us;
  band_productive_ = false;
  bands_[band_idx_].attempts++;
}

int32_t Scheduler::currentBandKhz() const {
  return band_count_ > 0 ? bands_[band_idx_].khz : 0;
}

std::size_t Scheduler::preferredBandIndex() const {
  std::size_t best = band_idx_;
  int best_n = -1;
  for (std::size_t i = 0; i < band_count_; ++i) {
    if (bands_[i].successes > best_n) {
      best_n = bands_[i].successes;
      best = i;
    }
  }
  return best;
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
  const std::size_t pref = preferredBandIndex();
  if (band_count_ > 0 && bands_[pref].successes > 0) band_idx_ = pref;
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
          (mono_us - last_listen_end_) >= cfg_.listen_interval_us) {
        enterListening(mono_us);
      }
      break;
    }

    case Phase::Listening: {
      if (want_serve_ ||
          (mono_us - listen_start_) >= cfg_.listen_duration_us) {
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
    if (snr > b.best_snr) b.best_snr = snr;
  }
}

void Scheduler::onWwvFix(int64_t mono_us) {
  has_fix_ = true;
  if (phase_ == Phase::Listening && cfg_.exit_listen_on_fix) {
    enterServing(mono_us);
  }
}

void Scheduler::onRdsFix(int64_t mono_us) {
  (void)mono_us;
  has_fix_ = true;  // acted on at the next tick (§5: serve on first fix)
}

void Scheduler::requestListenNow() { want_listen_ = true; }
void Scheduler::requestServeNow() { want_serve_ = true; }

}  // namespace airtime
