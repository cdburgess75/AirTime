#include <cstdint>

#include "airtime/station_bias.h"
#include "test_framework.h"

using airtime::StationBias;
using airtime::StationBiasConfig;
using airtime::StationBiasTable;

namespace {
constexpr int64_t kMs = 1000;
}

// A station's error is a constant, so repeated measurement should converge on
// it and then hold. Measured at the owner's QTH: 89.9 runs 37-87 ms late.
AT_TEST(bias_converges_on_a_constant_lateness) {
  StationBiasTable t;
  for (int i = 0; i < 12; ++i) t.observe(0xA920, 60 * kMs, i * 1000000);
  AT_CHECK(t.find(0xA920) != nullptr);
  AT_CHECK_NEAR((double)t.correction(0xA920), 60000.0, 500.0);
}

// Nothing is corrected on the strength of one sample: a single measurement
// taken while the clock happened to be off would otherwise start steering.
AT_TEST(bias_withholds_correction_until_confident) {
  StationBiasConfig cfg;
  cfg.min_samples = 3;
  StationBiasTable t(cfg);

  t.observe(0x1001, 300 * kMs, 0);
  AT_CHECK_EQ(t.correction(0x1001), 0);
  t.observe(0x1001, 300 * kMs, 1000000);
  AT_CHECK_EQ(t.correction(0x1001), 0);
  t.observe(0x1001, 300 * kMs, 2000000);
  AT_CHECK(t.correction(0x1001) != 0);   // third sample earns the trust
}

// Each station is measured on its own. The survey found three stations spread
// across 37-367 ms; averaging them into one number would help none of them.
AT_TEST(bias_is_per_station) {
  StationBiasTable t;
  for (int i = 0; i < 8; ++i) {
    t.observe(0xA920, 60 * kMs, i * 1000000);
    t.observe(0x6E47, 216 * kMs, i * 1000000);
    t.observe(0x33CB, 360 * kMs, i * 1000000);
  }
  AT_CHECK_NEAR((double)t.correction(0xA920), 60000.0, 2000.0);
  AT_CHECK_NEAR((double)t.correction(0x6E47), 216000.0, 4000.0);
  AT_CHECK_NEAR((double)t.correction(0x33CB), 360000.0, 6000.0);
}

// A station minutes out is broken, not biased. Correcting it would drag a
// nonsense reading into false agreement with the honest ones — exactly the
// stations the survey found sending times hours wrong. Let the voter bin them.
AT_TEST(bias_refuses_to_rehabilitate_a_broken_station) {
  StationBiasTable t;
  for (int i = 0; i < 10; ++i) t.observe(0xDEAD, 7LL * 1000000, i * 1000000);
  AT_CHECK_EQ(t.correction(0xDEAD), 0);
  AT_CHECK(t.find(0xDEAD) == nullptr);   // never even recorded
}

// One outlier must not undo hours of good measurements.
AT_TEST(bias_is_slow_to_be_moved_by_one_bad_sample) {
  StationBiasTable t;
  for (int i = 0; i < 20; ++i) t.observe(0x1001, 100 * kMs, i * 1000000);
  const int64_t settled = t.correction(0x1001);
  t.observe(0x1001, 900 * kMs, 21000000);          // one wild reading
  const int64_t after = t.correction(0x1001);
  AT_CHECK(after - settled < 250 * kMs);           // moved, but not far
}

// The table is small and the dial is not: when it fills, the entry that gets
// evicted must be one that was never trusted, never one that is steering.
AT_TEST(bias_evicts_the_least_measured_station) {
  StationBiasTable t;
  for (std::size_t s = 0; s < StationBiasTable::kMaxStations; ++s) {
    const uint16_t pi = (uint16_t)(0x1000 + s);
    const int n = (s == 0) ? 20 : 4;               // station 0 is best known
    for (int i = 0; i < n; ++i) t.observe(pi, 50 * kMs, i * 1000000);
  }
  t.observe(0x9999, 50 * kMs, 0);                  // one too many
  AT_CHECK(t.find(0x1000) != nullptr);             // the trusted one survived
  AT_CHECK(t.find(0x9999) != nullptr);             // the newcomer got a slot
}

// Biases are a property of the transmitter, so they are worth keeping across a
// power cycle rather than re-earned every boot.
AT_TEST(bias_can_be_seeded_from_storage) {
  StationBiasTable t;
  t.seed(0xA920, 87 * kMs, 10);
  AT_CHECK_EQ(t.correction(0xA920), 87 * kMs);   // trusted immediately
}
