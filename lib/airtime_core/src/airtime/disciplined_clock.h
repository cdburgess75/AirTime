#pragma once
//
// The disciplined internal clock — PLAN.md §4 arbiter rule 1: "The internal
// clock is the clock. Sources never step it directly; they steer rate and
// phase." This maps a monotonic time source (esp_timer µs on device; injected
// in tests) to a UTC estimate, with two controls:
//
//   * a rate correction (ppm) that compensates the crystal's frequency error
//     (learned over time — see drift.h), and
//   * phase steering that is *slewed* in gradually (never stepped), capped at a
//     maximum slew rate, exactly like a well-behaved NTP-disciplined clock.
//
// The one exception to "never step" is set(): the initial cold seed, where there
// is no prior time to preserve.
//
// Reads are pure closed-form functions of monotonic time (utcAt) so the model is
// deterministic and unit-testable; state transitions (set/steer/setRatePpm)
// consolidate the current estimate into a fresh base.

#include <cstdint>

namespace airtime {

class DisciplinedClock {
 public:
  // max_slew_ppm caps how fast phase corrections are slewed in (500 ppm matches
  // NTP's cap: 1 s of correction takes ~2000 s). Set <=0 to apply corrections
  // instantly (test convenience).
  explicit DisciplinedClock(int64_t max_slew_ppm = 500);

  bool isSet() const { return set_; }

  // Hard-set the clock (seed / confirmed reset). This is the only step.
  void set(int64_t mono_us, int64_t utc_us);

  // Steer phase by offset_us (= desired_utc - current clock utc). Positive means
  // the clock is behind and must catch up. Slewed in, never stepped.
  void steer(int64_t mono_us, int64_t offset_us);

  // Set the crystal rate correction in ppm (positive => clock runs faster).
  // Consolidates first so past time keeps the old rate and only the future uses
  // the new one (no discontinuity).
  void setRatePpm(int64_t mono_us, double ppm);
  double ratePpm() const { return rate_ppm_; }

  // Bake all accumulated rate + slew up to mono_us into the base (no observable
  // change to utcAt); used by the arbiter to realize injected slew before
  // measuring drift.
  void consolidateTo(int64_t mono_us);

  // UTC estimate (µs) at monotonic time mono_us. Pure/const.
  int64_t utcAt(int64_t mono_us) const;

  // Total slew actually injected over the clock's life (µs, signed). Used by the
  // drift estimator to separate frequency error from phase corrections.
  int64_t totalInjectedUs() const { return total_injected_; }

  int64_t slewRemainingUs(int64_t mono_us) const {
    return slew_offset_ - slewInjected(mono_us);
  }
  bool isSlewing(int64_t mono_us) const { return slewRemainingUs(mono_us) != 0; }

 private:
  int64_t slewInjected(int64_t mono_us) const;
  int64_t consolidate(int64_t mono_us);       // returns un-injected slew remainder
  void startSlew(int64_t mono_us, int64_t total_offset_us);

  bool set_ = false;
  int64_t base_mono_ = 0;
  int64_t base_utc_ = 0;
  double rate_ppm_ = 0.0;
  int64_t max_slew_ppm_;

  int64_t slew_start_mono_ = 0;
  int64_t slew_dur_ = 0;     // µs, > 0 while a ramp is active
  int64_t slew_offset_ = 0;  // signed total µs to inject over the ramp
  int64_t total_injected_ = 0;
};

}  // namespace airtime
