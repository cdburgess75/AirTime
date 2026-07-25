#pragma once
//
// Goertzel single-frequency power detector.
//
// PLAN.md §4 (WWV detection): "Goertzel filter at 1000 Hz on core 2, sampling
// IO11 ADC." This is the ~30-line reimplementation of the technique documented
// by HJBerndt. It detects the energy at one target frequency far more cheaply
// than a full FFT, which is exactly what the WWV 1000 Hz minute-marker needs.
//
// Usage: feed ADC samples one at a time via process(); every `block_size`
// samples a normalized power estimate is produced and the filter resets. The
// normalization is chosen so a full-scale sinusoid *at the target frequency*
// yields a power of amplitude^2 (i.e. 1.0 for a unit-amplitude tone),
// independent of block size — which makes thresholding intuitive.

#include <cstddef>
#include "types.h"

namespace airtime {

class Goertzel {
 public:
  // sample_rate_hz : ADC sampling rate
  // target_hz      : frequency to detect (1000.0 for WWV)
  // block_size     : samples per power estimate (sets frequency resolution and
  //                  update cadence; e.g. 800 @ 8 kHz => a 100 ms estimate)
  Goertzel(real sample_rate_hz, real target_hz, std::size_t block_size);

  // Feed one sample. Returns true exactly when a block completes; on true,
  // *power receives the normalized power for that block and the filter resets.
  bool process(real sample, real* power);

  // Convenience for tests / batch processing: run a whole buffer and return the
  // mean normalized power across the blocks that completed (0 if none did).
  real processBlock(const real* samples, std::size_t n);

  void reset();

  real coeff() const { return coeff_; }
  std::size_t blockSize() const { return block_; }

 private:
  real coeff_;
  std::size_t block_;
  std::size_t n_;
  real s1_, s2_;
};

}  // namespace airtime
