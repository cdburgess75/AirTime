#include "goertzel.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace airtime {

Goertzel::Goertzel(real sample_rate_hz, real target_hz, std::size_t block_size)
    : block_(block_size), n_(0), s1_(0), s2_(0) {
  // Use the exact target frequency (not the nearest integer bin) so arbitrary
  // sample-rate / frequency combinations behave sensibly.
  const real w =
      static_cast<real>(2.0 * M_PI * static_cast<double>(target_hz) /
                        static_cast<double>(sample_rate_hz));
  coeff_ = static_cast<real>(2.0) * std::cos(w);
}

void Goertzel::reset() {
  n_ = 0;
  s1_ = 0;
  s2_ = 0;
}

bool Goertzel::process(real sample, real* power) {
  const real s0 = sample + coeff_ * s1_ - s2_;
  s2_ = s1_;
  s1_ = s0;

  if (++n_ >= block_) {
    const real p = s1_ * s1_ + s2_ * s2_ - coeff_ * s1_ * s2_;
    if (power != nullptr) {
      // Normalize so a unit-amplitude tone at the target frequency reads ~1.0.
      // DFT bin magnitude of an amplitude-A sinusoid over N samples is A*N/2,
      // so p = (A*N/2)^2  =>  p * 4 / N^2 = A^2.
      const real nn = static_cast<real>(block_) * static_cast<real>(block_);
      *power = p * static_cast<real>(4.0) / nn;
    }
    reset();
    return true;
  }
  return false;
}

real Goertzel::processBlock(const real* samples, std::size_t n) {
  real sum = 0;
  int blocks = 0;
  real p = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (process(samples[i], &p)) {
      sum += p;
      ++blocks;
    }
  }
  return blocks != 0 ? sum / static_cast<real>(blocks) : static_cast<real>(0);
}

}  // namespace airtime
