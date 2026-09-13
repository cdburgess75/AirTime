#include "wwv_marker.h"

#include <algorithm>

namespace airtime {

bool wwvPhaseCorrection(int64_t clock_utc_at_edge_us, int64_t calibration_us,
                        int64_t* offset_us, int64_t max_offset_us) {
  constexpr int64_t kMinute = 60000000;

  // Back out the receive-chain latency: this is what our clock read at the
  // instant WWV actually transmitted the mark.
  const int64_t x = clock_utc_at_edge_us - calibration_us;

  // Distance to the nearest minute boundary, signed (how much to add to the
  // clock). Floor-mod keeps this correct for negative x too.
  int64_t rem = x % kMinute;
  if (rem < 0) rem += kMinute;
  const int64_t offset = (rem <= kMinute / 2) ? -rem : (kMinute - rem);

  if (offset > max_offset_us || offset < -max_offset_us) return false;
  if (offset_us != nullptr) *offset_us = offset;
  return true;
}

WwvMarkerDetector::WwvMarkerDetector(const WwvMarkerConfig& cfg) : cfg_(cfg) {}

void WwvMarkerDetector::reset() {
  in_tone_ = false;
  have_noise_ = false;
  noise_ = 0.0f;
  peak_ = 0.0f;
  tone_start_ = 0;
}

bool WwvMarkerDetector::process(int64_t mono_us, real power, WwvMarker* out) {
  bool emitted = false;

  if (power > diag_.max_power) diag_.max_power = power;

  if (!in_tone_) {
    // Track the noise floor only while idle, so a long tone can't inflate it.
    if (!have_noise_) {
      noise_ = power;
      have_noise_ = true;
    } else {
      noise_ += cfg_.noise_alpha * (power - noise_);
    }
    const real on_th = std::max(noise_ * cfg_.on_ratio, cfg_.min_power);
    if (power > on_th) {
      in_tone_ = true;
      tone_start_ = mono_us;  // leading edge == minute boundary
      peak_ = power;
      ++diag_.tone_starts;
    }
  } else {
    if (power > peak_) peak_ = power;
    const real off_th = std::max(noise_ * cfg_.off_ratio, cfg_.min_power * 0.5f);
    if (power < off_th) {
      const int64_t dur = mono_us - tone_start_;
      in_tone_ = false;
      diag_.last_tone_us = dur;
      if (dur > diag_.longest_tone_us) diag_.longest_tone_us = dur;
      if (dur >= cfg_.gate_min_us && dur <= cfg_.gate_max_us) {
        if (out != nullptr) {
          out->leading_edge_us = tone_start_;
          out->duration_us = dur;
          out->peak_power = peak_;
        }
        emitted = true;
        ++diag_.markers;
      } else if (dur < cfg_.gate_min_us) {
        ++diag_.rejected_short;
      } else {
        ++diag_.rejected_long;
      }
      // Resume noise tracking from this first silent sample.
      noise_ += cfg_.noise_alpha * (power - noise_);
    }
  }

  return emitted;
}

}  // namespace airtime
