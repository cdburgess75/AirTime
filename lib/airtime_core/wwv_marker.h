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
  real min_power = 0.01f;   // absolute floor so quiet noise can never trigger
  real noise_alpha = 0.05f; // EMA coefficient for noise-floor tracking (IDLE only)
  int64_t gate_min_us = 700000;  // accept marker durations in [700, 900] ms
  int64_t gate_max_us = 900000;
};

struct WwvMarker {
  int64_t leading_edge_us;  // minute-boundary timestamp (rising edge)
  int64_t duration_us;      // measured tone duration
  real peak_power;          // peak normalized power during the tone
};

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
