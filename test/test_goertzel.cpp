#include <cmath>
#include <vector>

#include "goertzel.h"
#include "test_framework.h"

using airtime::Goertzel;
using airtime::real;

namespace {

std::vector<real> tone(real fs, real f, real amp, std::size_t n) {
  std::vector<real> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = amp * static_cast<real>(std::sin(2.0 * M_PI * static_cast<double>(f) *
                                            static_cast<double>(i) /
                                            static_cast<double>(fs)));
  }
  return v;
}

}  // namespace

// A unit-amplitude tone at the target frequency normalizes to ~1.0.
AT_TEST(goertzel_unit_tone_reads_one) {
  const real fs = 8000, f = 1000;
  const std::size_t N = 800;  // 1000 Hz is exactly bin 100 => minimal leakage
  Goertzel g(fs, f, N);
  auto s = tone(fs, f, 1.0f, N);
  const real p = g.processBlock(s.data(), s.size());
  AT_CHECK_NEAR(p, 1.0, 0.05);
}

// Power scales as amplitude^2.
AT_TEST(goertzel_power_is_amplitude_squared) {
  const real fs = 8000, f = 1000;
  const std::size_t N = 800;
  Goertzel g(fs, f, N);
  auto s = tone(fs, f, 0.5f, N);
  const real p = g.processBlock(s.data(), s.size());
  AT_CHECK_NEAR(p, 0.25, 0.02);
}

// An off-target tone (600 Hz) is strongly rejected by the 1000 Hz detector.
AT_TEST(goertzel_rejects_off_frequency) {
  const real fs = 8000, f_target = 1000, f_other = 600;
  const std::size_t N = 800;
  Goertzel g(fs, f_target, N);
  auto s = tone(fs, f_other, 1.0f, N);
  const real p = g.processBlock(s.data(), s.size());
  AT_CHECK(p < 0.02);
}

// Silence reads ~0.
AT_TEST(goertzel_silence_is_zero) {
  const real fs = 8000, f = 1000;
  const std::size_t N = 800;
  Goertzel g(fs, f, N);
  std::vector<real> s(N, 0.0f);
  const real p = g.processBlock(s.data(), s.size());
  AT_CHECK_NEAR(p, 0.0, 1e-4);
}

// process() emits exactly one block per block_size samples and resets between.
AT_TEST(goertzel_block_boundary) {
  const real fs = 8000, f = 1000;
  const std::size_t N = 100;
  Goertzel g(fs, f, N);
  real p = -1;
  int emits = 0;
  auto s = tone(fs, f, 1.0f, 3 * N);
  for (real x : s) {
    if (g.process(x, &p)) ++emits;
  }
  AT_CHECK_EQ(emits, 3);
  AT_CHECK_NEAR(p, 1.0, 0.06);  // last block still ~unit power after resets
}
