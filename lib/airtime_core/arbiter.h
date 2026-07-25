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
  int64_t corroboration_window_us = 15LL * 60 * 1000000;    // window to corroborate a big jump across sources
  int64_t sync_threshold_us = 1000000;                      // "synced" while uncertainty < 1 s (FT8 needs < 1 s)
  double unc_growth_ppm = 20.0;                             // uncertainty growth from the crystal (±20 ppm spec)
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

  // Operator "confirm the big jump" action on the encoder. Consumed by the next
  // accepted large correction.
  void setOperatorConfirm(bool v) { operator_confirm_ = v; }

  // Read side.
  bool isSet() const { return clock_.isSet(); }
  int64_t utcAt(int64_t mono_us) const { return clock_.utcAt(mono_us); }
  int64_t uncertaintyUs(int64_t mono_us) const;
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

  int64_t last_accepted_mono_ = 0;
  int64_t last_offset_ = 0;
  int64_t last_injected_ = 0;
  int64_t last_sync_mono_ = 0;
  int64_t last_source_unc_ = 0;

  // Pending large-correction candidate awaiting a corroborating second source.
  bool pending_valid_ = false;
  Source pending_source_ = Source::None;
  int64_t pending_mono_ = 0;
  int64_t pending_utc_ = 0;
};

}  // namespace airtime
