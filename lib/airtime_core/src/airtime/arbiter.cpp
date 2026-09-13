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

  // A dateless WWV fix can place the clock exactly on a minute boundary but
  // can never say WHICH minute, so it must never be the source that
  // establishes one — not on a cold start, and not against a warm-boot memory
  // either. The gate keys on carries_date, not on the source: the 1000 Hz
  // marker is dateless and stops here; the 100 Hz timecode carries
  // minute/hour/day/year and passes, which is the entire reason it exists.
  //
  // Measured on the device, and the reason this is a hard gate rather than a
  // convention: a warm boot restored a time 65 minutes stale, the tuner sat on
  // AM through acquisition, and WWV got there first. It "corrected" the clock
  // by 0.28 s — perfectly, onto the wrong minute — and in doing so marked the
  // clock as sourced, which re-armed the corroboration gate against the very
  // RDS fix that knew the date. The laptop then read 3899.72 s out: 65 minutes
  // minus 0.28 s. A whole number of minutes is this bug's fingerprint.
  if (fix.source == Source::Wwv && !fix.carries_date && !source_synced_) {
    r.action = Action::Rejected;
    r.needs_confirmation = true;
    r.synced = isSynced(mono);
    r.uncertainty_us = uncertaintyUs(mono);
    return r;
  }

  // Rule 1 exception: cold seed of an unset clock — or of one holding nothing
  // but a restored memory, which is the same situation with a stale number in
  // it. See restore(): defending a warm-boot value against the first source
  // that actually heard the time is how a device that slept overnight stays
  // hours wrong all morning.
  if (!clock_.isSet() || !source_synced_) {
    // How far the memory turned out to be wrong, for the log. Zero on a genuine
    // cold start, where there was nothing to be wrong.
    const int64_t was_off = clock_.isSet() ? fix.utc_us - clock_.utcAt(mono) : 0;
    clock_.set(mono, fix.utc_us);
    source_synced_ = true;
    track_[si] = SourceTrack{true, mono, 0, clock_.totalInjectedUs()};
    last_sync_mono_ = mono;
    last_source_unc_ = fix.uncertainty_us;
    r.action = Action::Seeded;
    r.offset_us = was_off;
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
    // Too big to slew in any useful time? Then apply it outright. See
    // ArbiterConfig::step_apply_us — the alternative is a clock that creeps at
    // its slew ceiling for weeks while claiming to be synced. The gates above
    // already decided this correction is trustworthy.
    if (mag > cfg_.step_apply_us) {
      clock_.set(mono, fix.utc_us);
      // A step is a phase discontinuity, not evidence about frequency: the
      // interval that just ended was measured against a clock we have now
      // thrown away. Restart this source's rate measurement from here.
      track_[si] = SourceTrack{true, mono, 0, clock_.totalInjectedUs()};
      last_sync_mono_ = mono;
      last_source_unc_ = fix.uncertainty_us;
      operator_confirm_ = false;
      r.synced = isSynced(mono);
      r.uncertainty_us = uncertaintyUs(mono);
      return r;
    }

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
  source_synced_ = false;  // a memory; the first real fix re-seeds it
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

int64_t Arbiter::pendingCorrectionUs(int64_t mono_us) const {
  const int64_t p = clock_.slewRemainingUs(mono_us);
  return p < 0 ? -p : p;
}

bool Arbiter::isSynced(int64_t mono_us) const {
  return clock_.isSet() && uncertaintyUs(mono_us) < cfg_.sync_threshold_us;
}

}  // namespace airtime
