#include <cstdint>

#include "test_framework.h"
#include "airtime/wwv_marker.h"

using airtime::wwvPhaseCorrection;

namespace {
constexpr int64_t kMinute = 60000000;
// An exact minute boundary: 1783999980 = 60 × 29733333.
constexpr int64_t kBase = 1783999980LL * 1000000LL;
}  // namespace

// A clock that is 120 ms fast at the marker gets pulled back by 120 ms.
AT_TEST(wwvphase_pulls_back_fast_clock) {
  int64_t off = 0;
  AT_CHECK(wwvPhaseCorrection(kBase + 120000, 0, &off));
  AT_CHECK_EQ(off, -120000LL);
}

// A clock that is 120 ms slow gets pushed forward.
AT_TEST(wwvphase_pushes_slow_clock) {
  int64_t off = 0;
  AT_CHECK(wwvPhaseCorrection(kBase - 120000, 0, &off));
  AT_CHECK_EQ(off, 120000LL);
}

// An exactly-correct clock needs no correction.
AT_TEST(wwvphase_zero_when_aligned) {
  int64_t off = -1;
  AT_CHECK(wwvPhaseCorrection(kBase, 0, &off));
  AT_CHECK_EQ(off, 0LL);
}

// The calibration constant removes the receive chain's fixed latency: the tone
// is heard late, so an uncalibrated clock would be pushed late too.
AT_TEST(wwvphase_calibration_removes_chain_delay) {
  const int64_t delay = 25000;  // 25 ms of DSP + amp + ADC
  int64_t off = 0;

  // Clock is perfect; the marker is observed `delay` late.
  AT_CHECK(wwvPhaseCorrection(kBase + delay, delay, &off));
  AT_CHECK_EQ(off, 0LL);  // calibrated: no spurious correction

  // Without calibration the same observation would drag the clock 25 ms late.
  AT_CHECK(wwvPhaseCorrection(kBase + delay, 0, &off));
  AT_CHECK_EQ(off, -delay);
}

// It always locks to the NEAREST minute boundary, never the wrong one.
AT_TEST(wwvphase_locks_to_nearest_minute) {
  int64_t off = 0;
  // 40 s past the minute is 20 s short of the next one.
  AT_CHECK(wwvPhaseCorrection(kBase + 40 * 1000000, 0, &off));
  AT_CHECK_EQ(off, 20LL * 1000000);
  // 20 s past pulls back 20 s.
  AT_CHECK(wwvPhaseCorrection(kBase + 20 * 1000000, 0, &off));
  AT_CHECK_EQ(off, -20LL * 1000000);
}

// Beyond the acceptance window it refuses rather than guessing a minute.
AT_TEST(wwvphase_rejects_beyond_window) {
  int64_t off = 0;
  AT_CHECK(!wwvPhaseCorrection(kBase + 25 * 1000000, 0, &off, 10 * 1000000));
  AT_CHECK(wwvPhaseCorrection(kBase + 5 * 1000000, 0, &off, 10 * 1000000));
}

// The correction is bounded by half a minute by construction.
AT_TEST(wwvphase_bounded_by_half_minute) {
  for (int64_t d = 0; d < kMinute; d += 1000000) {
    int64_t off = 0;
    AT_CHECK(wwvPhaseCorrection(kBase + d, 0, &off));
    AT_CHECK(off <= kMinute / 2 && off >= -kMinute / 2);
    // Applying the correction always lands on an exact minute.
    AT_CHECK_EQ((kBase + d + off) % kMinute, 0LL);
  }
}
