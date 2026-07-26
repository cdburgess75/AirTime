#pragma once
//
// WWV minute-marker detector — PLAN.md §4 (WWV detection specifics):
//
//   * duration-gate the minute marker at 700-900 ms (WWV's marker is 800 ms of
//     1000 Hz at the top of each minute),
//   * threshold against a running noise-floor estimate so AGC pumping doesn't
//     shift the detected edge, and
//   * timestamp the LEADING edge — that rising edge is the minute boundary.
//
// It consumes the stream of normalized power estimates produced by the Goertzel
// detector (one per block, each stamped with a monotonic µs time). The continuous
// 500/600 Hz standard tones and the 440 Hz hourly tone are rejected upstream by
// the Goertzel's frequency selectivity; the duration gate here rejects both the
// short 5 ms seconds ticks and anything longer than a marker.

#include <cstdint>

#include "types.h"

namespace airtime {

struct WwvMarkerConfig {
  real on_ratio = 6.0f;     // enter TONE when power > noise_floor * on_ratio ...
  real off_ratio = 3.0f;    // ... and leave when power < noise_floor * off_ratio (hysteresis)
  // Absolute floor, so quiet noise can never trigger regardless of the ratio
  // test. MEASURED on the ATS Mini (Milestone 0 §6, samples normalised by half
  // ADC scale): in-bin noise power ~7.7e-5, a usable tone ~3.8e-4 to 1.9e-3.
  // This sits between them. The original 0.01 was a guess made before any
  // hardware existed and was ~5x ABOVE real signal — the detector would never
  // have fired. Worth re-validating against a genuine WWV marker in the field.
  real min_power = 2.0e-4f;
  real noise_alpha = 0.05f; // EMA coefficient for noise-floor tracking (IDLE only)
  int64_t gate_min_us = 700000;  // accept marker durations in [700, 900] ms
  int64_t gate_max_us = 900000;
};

struct WwvMarker {
  int64_t leading_edge_us;  // minute-boundary timestamp (rising edge)
  int64_t duration_us;      // measured tone duration
  real peak_power;          // peak normalized power during the tone
};

// Turn a detected minute marker into a phase correction.
//
// WWV gives phase, never date (PLAN.md §3), so this answers only "how far is the
// clock from the nearest minute boundary?" — which means the clock must already
// be roughly right (within half a minute) for the answer to be meaningful. A
// marker cannot cold-start the clock; RDS, warm-boot memory, or manual entry does
// that.
//
// clock_utc_at_edge_us : our UTC estimate at the marker's leading edge
// calibration_us       : fixed latency of the receive chain — SI4732 DSP group
//                        delay + amplifier + ADC (PLAN.md §4; measured once on
//                        hardware). The tone is observed this much late.
// max_offset_us        : reject beyond this (default 30 s — past half a minute we
//                        would be locking onto the wrong minute).
//
// Returns false if the implied correction exceeds max_offset_us.
bool wwvPhaseCorrection(int64_t clock_utc_at_edge_us, int64_t calibration_us,
                        int64_t* offset_us, int64_t max_offset_us = 30000000);

class WwvMarkerDetector {
 public:
  explicit WwvMarkerDetector(const WwvMarkerConfig& cfg = WwvMarkerConfig{});

  // Feed one Goertzel power estimate at monotonic time mono_us. Returns true and
  // fills *out exactly on the falling edge of a duration-valid marker.
  bool process(int64_t mono_us, real power, WwvMarker* out);

  void reset();

  real noiseFloor() const { return noise_; }
  bool inTone() const { return in_tone_; }

 private:
  WwvMarkerConfig cfg_;
  bool in_tone_ = false;
  bool have_noise_ = false;
  real noise_ = 0.0f;
  real peak_ = 0.0f;
  int64_t tone_start_ = 0;
};

}  // namespace airtime
