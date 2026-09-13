#pragma once
//
// Crystal drift learning — PLAN.md §4 arbiter rule 4: "Learn the crystal. Track
// rate error across successive syncs; store correction in NVS. A characterized
// ±20 ppm crystal behaves like a much better one; this is what makes hourly (not
// constant) syncing sufficient."
//
// This is a clamped leaky integrator on the *residual* frequency error. At each
// time fix the arbiter measures how much the clock drifted over the interval,
// with the phase corrections it deliberately slewed in added back (so only the
// genuine frequency error remains — see disciplined_clock.totalInjectedUs). The
// estimator integrates that residual into a learned ppm that drives the clock's
// rate correction toward the true crystal error.

#include <cstdint>

namespace airtime {

class DriftEstimator {
 public:
  // gain: integrator gain (0..1); 0.5 converges in a handful of fixes.
  // max_ppm: clamp on the learned correction (crystal spec is ±20 ppm; allow
  //          margin for aging/temperature).
  explicit DriftEstimator(double gain = 0.5, double max_ppm = 100.0);

  // Integrate a directly-measured residual frequency error (ppm). Returns the
  // updated learned ppm. This is the primitive the arbiter calls.
  double integrate(double residual_ppm);

  // Standalone convenience: derive the residual from a fix at monotonic mono_us
  // with pre-correction offset offset_us (= true_utc - clock_utc), given the
  // slew injected since the previous fix. Returns the updated learned ppm.
  double update(int64_t mono_us, int64_t offset_us, int64_t injected_since_prev_us);

  double ppm() const { return ppm_; }
  bool hasEstimate() const { return samples_ > 0; }
  int samples() const { return samples_; }

  // Magnitude of the rate error still unexplained, as an EMA of |residual|.
  // This is what §4 rule 4 buys: once the crystal is characterised the clock's
  // rate is known far better than its ±20 ppm spec, so uncertainty between syncs
  // should grow at THIS rate, not at the datasheet number. Returns a large value
  // until enough fixes have landed to mean anything.
  double residualPpm() const { return samples_ >= 3 ? residual_ppm_ : 1e9; }

  // Seed from persisted NVS value.
  void setPpm(double p);
  void reset();

 private:
  double clampPpm(double p) const;

  double ppm_ = 0.0;
  double residual_ppm_ = 0.0;
  double gain_;
  double max_ppm_;
  bool have_prev_ = false;
  int64_t prev_mono_ = 0;
  int64_t prev_offset_ = 0;
  int samples_ = 0;
};

}  // namespace airtime
