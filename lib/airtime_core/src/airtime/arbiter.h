#pragma once
//
// The arbiter — "the heart of the project" (PLAN.md §4). It turns time fixes from
// the sources (RDS voting, WWV phase, manual) into disciplined corrections of the
// internal clock, and it owns the honest uncertainty state of §5.
//
// Rules implemented (PLAN.md §4):
//   1. The internal clock is the clock; sources steer rate/phase, never step it
//      (except the initial cold seed).
//   2. Corrections < 500 ms: accept from any single credible source; slew.
//   3. Corrections >= 500 ms: require two independent sources agreeing within
//      tolerance, OR explicit operator confirmation. Otherwise reject and flag
//      that confirmation is needed.
//   4. Learn the crystal (drift.h) and apply the learned rate to the clock.
//   5. Uncertainty is first-class: ±(time since last sync × drift rate + source
//      uncertainty); served time is flagged unsynchronized past a threshold.

#include <cstdint>

#include "disciplined_clock.h"
#include "drift.h"
#include "types.h"

namespace airtime {

enum class Action {
  Seeded,       // cold seed of an unset clock
  Slewed,       // small correction accepted and slewed
  SlewedLarge,  // large correction accepted (corroborated / confirmed) and slewed
  Rejected,     // large correction withheld pending corroboration/confirmation
};

struct TimeFix {
  Source source = Source::None;
  int64_t mono_us = 0;          // monotonic time the fix is valid at
  int64_t utc_us = 0;           // asserted UTC (µs since 1970)
  int64_t uncertainty_us = 0;   // source's own ± (e.g. RDS ~200 ms, WWV ~20 ms)
  int independent_support = 1;  // independent sources backing it (RDS voting => agreeing stations)
};

struct ArbiterConfig {
  int64_t step_threshold_us = 500000;                       // 500 ms (rule 2/3 boundary)
  // Above this, an ACCEPTED correction is applied as a step instead of a slew.
  //
  // Rule 1 says slew, never step — but a slew is capped at max_slew_ppm, so the
  // time to apply a correction is offset / 500 ppm = offset x 2000. A 2 s error
  // takes 67 minutes. An hour's error takes 87 DAYS, during which the clock
  // creeps toward truth at its ceiling and reports itself synced to ±110 ms the
  // whole way. That is not conservatism, it is a lie with a slow leak.
  //
  // Measured on the device, and the reason this exists: a warm boot restored an
  // NVS time from an hour-old power-down; RDS immediately and correctly asked
  // for +3765 s; the arbiter accepted it; and the clock was still 3764 s out
  // 38 minutes later, closing at 563 ppm. Three separate signatures in one log
  // (device-vs-laptop offset, the WWV phase 3765 s mod 60 = 15 s, and the
  // 563 ppm closing rate) all agreed.
  //
  // The accept/reject gates are untouched: a large correction still needs two
  // agreeing sources or an operator. This only decides HOW an already-accepted
  // correction is applied. Below the threshold a slew is still strictly better —
  // NTP clients never see time go backwards.
  int64_t step_apply_us = 2000000;                          // 2 s (~67 min of slewing)
  int64_t corroboration_window_us = 15LL * 60 * 1000000;    // window to corroborate a big jump across sources
  int64_t sync_threshold_us = 1000000;                      // "synced" while uncertainty < 1 s (FT8 needs < 1 s)
  // Uncertainty growth between syncs. Starts at the crystal's ±20 ppm spec, but
  // once the drift estimator has characterised the crystal the clock's rate is
  // known far better than that, and growth follows the measured residual instead
  // — which is precisely what §4 rule 4 means by "a characterised ±20 ppm
  // crystal behaves like a much better one".
  //
  // This is not cosmetic. Overstating growth inflates our uncertainty between
  // WWV windows, which raises a coarse source's blend gain and lets RDS drag
  // phase back off the minute. Measured: with growth pinned at 20 ppm the error
  // sawtoothed between -25 ms (just after WWV) and -204 ms (an hour later).
  double unc_growth_ppm = 20.0;
  double min_unc_growth_ppm = 0.5;   // floor; we never claim perfect knowledge

  // Weight each accepted correction by how much better the source is than our
  // current estimate: gain = our_var / (our_var + source_var). This makes §4
  // rule 5's "uncertainty is first-class" load-bearing, and produces §4's source
  // tiering without a hardcoded priority list — a coarse source barely moves a
  // well-disciplined clock, while any source pulls hard on a stale one.
  //
  // Without it the source that reports MORE OFTEN wins regardless of precision:
  // RDS at ±250 ms every ~75 s simply overrides WWV at ±30 ms once an hour, and
  // Milestone 3's phase lock is undone as fast as it is applied.
  //
  // The accept/reject gates above are untouched — this only scales a correction
  // that has already been accepted.
  bool uncertainty_weighting = true;
  // Floor on the posterior, so repeated fixes cannot make the clock so
  // "certain" that it stops responding. We cannot know UTC better than the
  // calibration constant is accurate anyway.
  int64_t min_uncertainty_us = 5000;
  double drift_gain = 0.5;
  double drift_max_ppm = 100.0;
  int64_t max_slew_ppm = 500;
};

struct ArbiterUpdate {
  Action action = Action::Rejected;
  int64_t offset_us = 0;            // pre-correction offset (true - clock)
  bool synced = false;
  int64_t uncertainty_us = 0;
  bool needs_confirmation = false;  // set when a large correction was withheld
};

class Arbiter {
 public:
  explicit Arbiter(const ArbiterConfig& cfg = ArbiterConfig{});

  // Feed a time fix; returns what was done and the resulting state.
  ArbiterUpdate update(const TimeFix& fix);

  // Warm boot: seed the clock from persisted last-known time WITHOUT claiming a
  // sync. `uncertainty_us` should reflect how stale the stored value is, so the
  // device honestly reports "UNSYNCED — last-known + drift" (§5) and NTP keeps
  // flagging itself unusable until a real source arrives. Enough to let WWV
  // phase-lock work, which needs the clock within half a minute.
  //
  // A restored clock is a MEMORY, not a measurement, and the first real fix
  // re-seeds it outright (see update()): there is nothing here worth defending
  // against a source that actually heard the time. Without that, a device
  // switched off overnight wakes up hours wrong and the corroboration gate
  // protects the wrong value.
  void restore(int64_t mono_us, int64_t utc_us, int64_t uncertainty_us);

  // Seed the learned crystal correction from NVS (PLAN.md §4 rule 4) so a
  // characterized crystal keeps its characterization across a power cycle.
  void seedDriftPpm(int64_t mono_us, double ppm);

  // Operator "confirm the big jump" action on the encoder. Consumed by the next
  // accepted large correction.
  void setOperatorConfirm(bool v) { operator_confirm_ = v; }

  // Read side.
  bool isSet() const { return clock_.isSet(); }
  int64_t utcAt(int64_t mono_us) const { return clock_.utcAt(mono_us); }

  // The source-and-drift uncertainty model of §4 rule 5. This is also what
  // weights every correction (see uncertainty_weighting), so it must describe
  // how good our ESTIMATE is — nothing else belongs in it.
  int64_t uncertaintyUs(int64_t mono_us) const;

  // Correction already accepted but not yet slewed in: error we know we still
  // carry, with a known sign. Deliberately NOT part of uncertaintyUs — folding
  // it in raises a coarse source's blend gain, so a biased station gets to pull
  // hardest exactly while a better source's correction is landing, and undoes
  // it (measured: WWV's fix reverted within ten minutes, every time). It is
  // reported to the operator and to NTP clients instead, where it is honest
  // without being in the loop.
  int64_t pendingCorrectionUs(int64_t mono_us) const;
  bool isSynced(int64_t mono_us) const;
  double ratePpm() const { return clock_.ratePpm(); }

  const DisciplinedClock& clock() const { return clock_; }
  const DriftEstimator& drift() const { return drift_; }

 private:
  bool corroborate(const TimeFix& fix, int64_t mono_us);

  ArbiterConfig cfg_;
  DisciplinedClock clock_;
  DriftEstimator drift_;

  bool operator_confirm_ = false;

  // Drift bookkeeping is PER SOURCE, and that is load-bearing (§4 rule 4).
  // A frequency error is only observable by watching ONE source's offset
  // evolve over time. Comparing an RDS fix against a WWV fix measures the two
  // sources' MUTUAL BIAS — a constant — and dividing a constant by the elapsed
  // time between them manufactures a rate error out of nothing.
  //
  // Measured, with a station biased 700 ms and WWV landing 30 s later: a
  // correctly learned +17.9 ppm was destroyed by the first WWV fix (0.7 s /
  // 30 s = 23000 ppm) and the estimator sat on its ±100 ppm rail from then on,
  // costing ~285 ms an hour — worse than leaving the crystal uncorrected. The
  // poisoned value is persisted to NVS, so it survives a power cycle too.
  struct SourceTrack {
    bool have = false;
    int64_t mono = 0;
    int64_t offset = 0;
    int64_t injected = 0;
  };
  SourceTrack track_[4];  // indexed by Source

  // False until a real source has been accepted since power-on. A clock that is
  // only `restore()`d has this false, which is what makes the first fix re-seed.
  bool source_synced_ = false;

  int64_t last_sync_mono_ = 0;
  int64_t last_source_unc_ = 0;

  // Pending large-correction candidate awaiting a corroborating second source.
  bool pending_valid_ = false;
  Source pending_source_ = Source::None;
  int64_t pending_mono_ = 0;
  int64_t pending_utc_ = 0;
};

}  // namespace airtime
