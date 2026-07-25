#include <cstdint>

#include "disciplined_clock.h"
#include "test_framework.h"

using airtime::DisciplinedClock;

// A freshly set clock reads back exactly.
AT_TEST(clock_set_reads_back) {
  DisciplinedClock c;
  c.set(0, 1000000);
  AT_CHECK(c.isSet());
  AT_CHECK_EQ(c.utcAt(0), 1000000LL);
}

// With zero rate, UTC advances 1:1 with monotonic time.
AT_TEST(clock_rate_zero_one_to_one) {
  DisciplinedClock c;
  c.set(0, 1000000);
  AT_CHECK_EQ(c.utcAt(1000000), 2000000LL);
}

// A positive rate makes the clock run faster by exactly ppm.
AT_TEST(clock_rate_positive_faster) {
  DisciplinedClock c;
  c.set(0, 1000000);
  c.setRatePpm(0, 100.0);  // +100 ppm
  // over 1e6 us, extra = 1e6 * 100e-6 = 100 us
  AT_CHECK_EQ(c.utcAt(1000000), 2000100LL);
}

// steer() applies as a smooth slew, never an instantaneous step.
AT_TEST(clock_steer_slews_not_steps) {
  DisciplinedClock c(500);  // 500 ppm max slew
  c.set(0, 1000000);
  c.steer(0, 300000);  // clock is 300 ms behind

  // No step: at the instant of steering the reading is unchanged.
  AT_CHECK_EQ(c.utcAt(0), 1000000LL);
  AT_CHECK_EQ(c.slewRemainingUs(0), 300000LL);

  // 300 ms at 500 ppm slews over 600 s. Halfway (~300 s) about half is in.
  const int64_t mid = c.utcAt(300000000) - (1000000LL + 300000000LL);
  AT_CHECK(mid > 140000 && mid < 160000);

  // Well past the ramp the full correction is applied and holds.
  AT_CHECK_EQ(c.utcAt(1200000000) - (1000000LL + 1200000000LL), 300000LL);
  AT_CHECK_EQ(c.slewRemainingUs(1200000000), 0LL);
}

// Changing the rate mid-flight does not create a discontinuity.
AT_TEST(clock_setrate_no_jump) {
  DisciplinedClock c;
  c.set(0, 1000000);
  const int64_t before = c.utcAt(1000000);
  c.setRatePpm(1000000, 50.0);
  const int64_t after = c.utcAt(1000000);
  AT_CHECK_EQ(before, after);  // no jump at the switch instant
  // future advances at the new rate: +1e6 us * (1 + 50e-6) = +1000050
  AT_CHECK_EQ(c.utcAt(2000000), after + 1000050);
}

// Monotonic time source never set => reads are inert but safe.
AT_TEST(clock_unset_is_safe) {
  DisciplinedClock c;
  AT_CHECK(!c.isSet());
  AT_CHECK_EQ(c.utcAt(123456), 0LL);
}
