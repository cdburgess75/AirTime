#include <cmath>
#include <cstdint>

#include "drift.h"
#include "test_framework.h"

using airtime::DriftEstimator;

// Open-loop: a clock running slow by E ppm should drive the learned rate to +E.
AT_TEST(drift_converges_to_true_error) {
  const double E = 30.0;  // crystal slow by 30 ppm
  DriftEstimator d(0.5, 100.0);
  const int64_t hour = 3600LL * 1000000;

  int64_t mono = 0;
  double offset = 0.0;
  d.update(mono, 0, 0);  // establish the first reference point

  for (int k = 1; k <= 30; ++k) {
    // Between fixes the uncorrected offset grows at (E - applied_ppm).
    offset += (E - d.ppm()) * static_cast<double>(hour) / 1e6;
    mono += hour;
    d.update(mono, static_cast<int64_t>(llround(offset)), 0);
  }
  AT_CHECK_NEAR(d.ppm(), 30.0, 0.5);
}

// The learned value is clamped to +/- max_ppm.
AT_TEST(drift_clamps) {
  DriftEstimator d(1.0, 20.0);
  d.integrate(1000.0);
  AT_CHECK_NEAR(d.ppm(), 20.0, 1e-9);
  d.integrate(-5000.0);
  AT_CHECK_NEAR(d.ppm(), -20.0, 1e-9);
}

// Slew injected between fixes contributes to the frequency estimate even when
// the net offset stays flat (the phase correction masked the drift).
AT_TEST(drift_injected_counts) {
  DriftEstimator d(0.5, 100.0);
  const int64_t hour = 3600LL * 1000000;
  d.update(0, 0, 0);
  d.update(hour, 0, 90000);  // offset flat, but 90 ms was slewed in
  AT_CHECK(d.ppm() > 0.0);
}

// A brand-new estimator reports no estimate and zero ppm.
AT_TEST(drift_initial_state) {
  DriftEstimator d;
  AT_CHECK(!d.hasEstimate());
  AT_CHECK_NEAR(d.ppm(), 0.0, 1e-12);
}
