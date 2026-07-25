#include "wwv_marker.h"

#include <algorithm>

namespace airtime {

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
    }
  } else {
    if (power > peak_) peak_ = power;
    const real off_th = std::max(noise_ * cfg_.off_ratio, cfg_.min_power * 0.5f);
    if (power < off_th) {
      const int64_t dur = mono_us - tone_start_;
      in_tone_ = false;
      if (dur >= cfg_.gate_min_us && dur <= cfg_.gate_max_us) {
        if (out != nullptr) {
          out->leading_edge_us = tone_start_;
          out->duration_us = dur;
          out->peak_power = peak_;
        }
        emitted = true;
      }
      // Resume noise tracking from this first silent sample.
      noise_ += cfg_.noise_alpha * (power - noise_);
    }
  }

  return emitted;
}

}  // namespace airtime
