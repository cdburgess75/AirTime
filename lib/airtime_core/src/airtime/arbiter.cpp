#include "arbiter.h"

#include <cmath>

namespace airtime {

static inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static inline int sourceIndex(Source s) {
  const int i = static_cast<int>(s);
  return (i >= 0 && i < 4) ? i : 0;
}

Arbiter::Arbiter(const ArbiterConfig& cfg)
    : cfg_(cfg),
      clock_(cfg.max_slew_ppm),
      drift_(cfg.drift_gain, cfg.drift_max_ppm) {}

bool Arbiter::corroborate(const TimeFix& fix, int64_t mono_us) {
  // Accept if a recent pending candidate from a DIFFERENT source agrees about
  // UTC now (advance its assertion by the elapsed real time).
  if (pending_valid_ && (mono_us - pending_mono_) <= cfg_.corroboration_window_us &&
      pending_source_ != fix.source) {
    const int64_t pending_now = pending_utc_ + (mono_us - pending_mono_);
    if (iabs64(pending_now - fix.utc_us) <= cfg_.step_threshold_us) {
      pending_valid_ = false;
      return true;
    }
  }
  // Otherwise remember this as the candidate and wait for a second source.
  pending_valid_ = true;
  pending_source_ = fix.source;
  pending_mono_ = mono_us;
  pending_utc_ = fix.utc_us;
  return false;
}

ArbiterUpdate Arbiter::update(const TimeFix& fix) {
  ArbiterUpdate r{};
  const int64_t mono = fix.mono_us;

  const int si = sourceIndex(fix.source);

  // Rule 1 exception: cold seed of an unset clock.
  if (!clock_.isSet()) {
    clock_.set(mono, fix.utc_us);
    track_[si] = SourceTrack{true, mono, 0, clock_.totalInjectedUs()};
    last_sync_mono_ = mono;
    last_source_unc_ = fix.uncertainty_us;
    r.action = Action::Seeded;
    r.offset_us = 0;
    r.synced = isSynced(mono);
    r.uncertainty_us = uncertaintyUs(mono);
    return r;
  }

  // Realize any slew injected since the last fix, then measure the offset.
  clock_.consolidateTo(mono);
  const int64_t predicted = clock_.utcAt(mono);
  const int64_t offset = fix.utc_us - predicted;
  const int64_t mag = iabs64(offset);
  r.offset_us = offset;

  bool accept;
  if (mag < cfg_.step_threshold_us) {
    accept = true;                      // rule 2
    r.action = Action::Slewed;
  } else {                              // rule 3
    const bool corrob = fix.independent_support >= 2 || operator_confirm_ ||
                        corroborate(fix, mono);
    if (corrob) {
      accept = true;
      r.action = Action::SlewedLarge;
    } else {
      accept = false;
      r.action = Action::Rejected;
      r.needs_confirmation = true;
    }
  }

  if (accept) {
    // Rule 4: learn the crystal from the residual frequency error, adding back
    // the slew we deliberately injected so only genuine drift is measured.
    // Measured against THIS source's own previous fix — see SourceTrack.
    const SourceTrack& prev = track_[si];
    const int64_t dmono = mono - prev.mono;
    if (prev.have && dmono > 0) {
      const int64_t injected = clock_.totalInjectedUs() - prev.injected;
      const double residual_ppm =
          (static_cast<double>(offset - prev.offset) +
           static_cast<double>(injected)) /
          static_cast<double>(dmono) * 1e6;
      const double ppm = drift_.integrate(residual_ppm);
      clock_.setRatePpm(mono, ppm);
    }

    // Rule 5, made load-bearing: blend rather than obey. `offset` remains the
    // full measurement (the drift estimator needs it); only the amount actually
    // applied is scaled.
    int64_t applied = offset;
    int64_t posterior = fix.uncertainty_us;
    if (cfg_.uncertainty_weighting) {
      const double ours = static_cast<double>(uncertaintyUs(mono));
      const double src =
          static_cast<double>(fix.uncertainty_us > 0 ? fix.uncertainty_us : 1);
      const double ov = ours * ours;
      const double sv = src * src;
      const double gain = ov / (ov + sv);
      applied = static_cast<int64_t>(llround(gain * static_cast<double>(offset)));
      // Posterior of two independent estimates. Note this can only ever shrink
      // our uncertainty — before, a coarse fix arriving after a precise one made
      // the device report itself *less* certain than it had been, which is
      // backwards and fed straight into the NTP root dispersion.
      posterior = static_cast<int64_t>(llround(std::sqrt(1.0 / (1.0 / ov + 1.0 / sv))));
      if (posterior < cfg_.min_uncertainty_us) posterior = cfg_.min_uncertainty_us;
    }

    clock_.steer(mono, applied);        // slew, never step (rule 1)

    track_[si] = SourceTrack{true, mono, offset, clock_.totalInjectedUs()};
    last_sync_mono_ = mono;
    last_source_unc_ = posterior;
    operator_confirm_ = false;          // confirmation is single-use
  }

  r.synced = isSynced(mono);
  r.uncertainty_us = uncertaintyUs(mono);
  return r;
}

void Arbiter::restore(int64_t mono_us, int64_t utc_us, int64_t uncertainty_us) {
  clock_.set(mono_us, utc_us);
  // No source has spoken yet: leave every track empty so the first real fix
  // from each source starts a clean rate measurement rather than differencing
  // against a remembered time.
  for (SourceTrack& t : track_) t = SourceTrack{};
  last_sync_mono_ = mono_us;
  // Deliberately large: this is a memory, not a measurement. isSynced() stays
  // false until a real source lands.
  last_source_unc_ = uncertainty_us;
}

void Arbiter::seedDriftPpm(int64_t mono_us, double ppm) {
  drift_.setPpm(ppm);
  clock_.setRatePpm(mono_us, drift_.ppm());
}

int64_t Arbiter::uncertaintyUs(int64_t mono_us) const {
  if (!clock_.isSet()) return INT64_MAX / 4;
  int64_t dt = mono_us - last_sync_mono_;
  if (dt < 0) dt = 0;
  // Grow at the measured residual rate error once the crystal is characterised,
  // falling back to its datasheet spec until then (§4 rule 4).
  double rate_ppm = cfg_.unc_growth_ppm;
  const double residual = drift_.residualPpm();
  if (residual < rate_ppm) rate_ppm = residual;
  if (rate_ppm < cfg_.min_unc_growth_ppm) rate_ppm = cfg_.min_unc_growth_ppm;
  const double growth = static_cast<double>(dt) * rate_ppm / 1e6;
  return last_source_unc_ + static_cast<int64_t>(llround(growth));
}

bool Arbiter::isSynced(int64_t mono_us) const {
  return clock_.isSet() && uncertaintyUs(mono_us) < cfg_.sync_threshold_us;
}

}  // namespace airtime
