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
  if (band_count_ > 0) bands_[band_idx_].attempts++;
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
  band_idx_ = preferredBandIndex();
  if (band_count_ > 0) bands_[band_idx_].attempts++;
  want_listen_ = false;
  want_serve_ = false;
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
      if (band_count_ > 0 && (mono_us - band_start_) >= cfg_.band_dwell_us) {
        band_idx_ = (band_idx_ + 1) % band_count_;
        band_start_ = mono_us;
        bands_[band_idx_].attempts++;
      }
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
      if (band_count_ > 0 && (mono_us - band_start_) >= cfg_.band_dwell_us) {
        band_idx_ = (band_idx_ + 1) % band_count_;
        band_start_ = mono_us;
        bands_[band_idx_].attempts++;
      }
      break;
    }
  }

  return directive();
}

void Scheduler::onWwvFix(int64_t mono_us, real snr) {
  has_fix_ = true;
  if (band_count_ > 0) {
    BandStats& b = bands_[band_idx_];
    b.successes++;
    if (snr > b.best_snr) b.best_snr = snr;
  }
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
