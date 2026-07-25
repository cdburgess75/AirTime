#include "drift.h"

namespace airtime {

DriftEstimator::DriftEstimator(double gain, double max_ppm)
    : gain_(gain), max_ppm_(max_ppm) {}

double DriftEstimator::clampPpm(double p) const {
  if (p > max_ppm_) return max_ppm_;
  if (p < -max_ppm_) return -max_ppm_;
  return p;
}

double DriftEstimator::integrate(double residual_ppm) {
  ppm_ = clampPpm(ppm_ + gain_ * residual_ppm);
  ++samples_;
  return ppm_;
}

double DriftEstimator::update(int64_t mono_us, int64_t offset_us,
                              int64_t injected_since_prev_us) {
  if (have_prev_) {
    const int64_t dmono = mono_us - prev_mono_;
    if (dmono > 0) {
      const double residual_ppm =
          (static_cast<double>(offset_us - prev_offset_) +
           static_cast<double>(injected_since_prev_us)) /
          static_cast<double>(dmono) * 1e6;
      integrate(residual_ppm);
    }
  }
  prev_mono_ = mono_us;
  prev_offset_ = offset_us;
  have_prev_ = true;
  return ppm_;
}

void DriftEstimator::setPpm(double p) { ppm_ = clampPpm(p); }

void DriftEstimator::reset() {
  ppm_ = 0.0;
  have_prev_ = false;
  prev_mono_ = 0;
  prev_offset_ = 0;
  samples_ = 0;
}

}  // namespace airtime
