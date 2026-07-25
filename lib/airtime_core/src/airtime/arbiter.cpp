#include "arbiter.h"

#include <cmath>

namespace airtime {

static inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

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

  // Rule 1 exception: cold seed of an unset clock.
  if (!clock_.isSet()) {
    clock_.set(mono, fix.utc_us);
    last_accepted_mono_ = mono;
    last_offset_ = 0;
    last_injected_ = clock_.totalInjectedUs();
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
    const int64_t dmono = mono - last_accepted_mono_;
    const int64_t injected = clock_.totalInjectedUs() - last_injected_;
    if (dmono > 0) {
      const double residual_ppm =
          (static_cast<double>(offset - last_offset_) +
           static_cast<double>(injected)) /
          static_cast<double>(dmono) * 1e6;
      const double ppm = drift_.integrate(residual_ppm);
      clock_.setRatePpm(mono, ppm);
    }

    clock_.steer(mono, offset);         // slew, never step (rule 1)

    last_accepted_mono_ = mono;
    last_offset_ = offset;
    last_injected_ = clock_.totalInjectedUs();
    last_sync_mono_ = mono;
    last_source_unc_ = fix.uncertainty_us;
    operator_confirm_ = false;          // confirmation is single-use
  }

  r.synced = isSynced(mono);
  r.uncertainty_us = uncertaintyUs(mono);
  return r;
}

void Arbiter::restore(int64_t mono_us, int64_t utc_us, int64_t uncertainty_us) {
  clock_.set(mono_us, utc_us);
  last_accepted_mono_ = mono_us;
  last_offset_ = 0;
  last_injected_ = clock_.totalInjectedUs();
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
  const double growth = static_cast<double>(dt) * cfg_.unc_growth_ppm / 1e6;
  return last_source_unc_ + static_cast<int64_t>(llround(growth));
}

bool Arbiter::isSynced(int64_t mono_us) const {
  return clock_.isSet() && uncertaintyUs(mono_us) < cfg_.sync_threshold_us;
}

}  // namespace airtime
