#include <cmath>
#include <cstdint>

#include "airtime/arbiter.h"
#include "test_framework.h"

using airtime::Action;
using airtime::Arbiter;
using airtime::ArbiterConfig;
using airtime::ArbiterUpdate;
using airtime::Source;
using airtime::TimeFix;

namespace {
int64_t iabs(int64_t v) { return v < 0 ? -v : v; }
TimeFix fix(Source s, int64_t mono, int64_t utc, int64_t unc, int support) {
  TimeFix f;
  f.source = s;
  f.mono_us = mono;
  f.utc_us = utc;
  f.uncertainty_us = unc;
  f.independent_support = support;
  return f;
}
constexpr int64_t T0 = 1000000000000000LL;  // arbitrary UTC epoch, µs
}  // namespace

// The first fix seeds the clock (the one allowed step).
AT_TEST(arb_seed_on_first_fix) {
  Arbiter a;
  const ArbiterUpdate r = a.update(fix(Source::Rds, 1000, T0, 200000, 1));
  AT_CHECK(r.action == Action::Seeded);
  AT_CHECK(a.isSet());
  AT_CHECK_EQ(a.utcAt(1000), T0);
}

// A sub-500 ms correction is slewed, not stepped (rule 2).
AT_TEST(arb_small_correction_slews) {
  Arbiter a;
  a.update(fix(Source::Rds, 0, T0, 200000, 1));  // seed at mono 0
  // 1 s later, source says clock is 100 ms behind.
  const ArbiterUpdate r =
      a.update(fix(Source::Rds, 1000000, T0 + 1000000 + 100000, 200000, 1));
  AT_CHECK(r.action == Action::Slewed);
  AT_CHECK_EQ(r.offset_us, 100000LL);
  // No step: reading at the fix instant is still the predicted value.
  AT_CHECK_EQ(a.utcAt(1000000), T0 + 1000000);
}

// A >=500 ms correction from a single unsupported source is withheld (rule 3).
AT_TEST(arb_large_single_source_rejected) {
  Arbiter a;
  a.update(fix(Source::Rds, 0, T0, 200000, 1));
  const ArbiterUpdate r =
      a.update(fix(Source::Rds, 1000000, T0 + 1000000 + 800000, 200000, 1));
  AT_CHECK(r.action == Action::Rejected);
  AT_CHECK(r.needs_confirmation);
  AT_CHECK_EQ(a.utcAt(1000000), T0 + 1000000);  // clock untouched
}

// Operator confirmation lets a large correction through (rule 3).
AT_TEST(arb_large_operator_confirm) {
  Arbiter a;
  a.update(fix(Source::Rds, 0, T0, 200000, 1));
  a.setOperatorConfirm(true);
  const ArbiterUpdate r =
      a.update(fix(Source::Rds, 1000000, T0 + 1000000 + 800000, 200000, 1));
  AT_CHECK(r.action == Action::SlewedLarge);
}

// Two independent stations backing one fix (RDS voting) clears the bar (rule 3).
AT_TEST(arb_large_rds_consensus) {
  Arbiter a;
  a.update(fix(Source::Rds, 0, T0, 200000, 1));
  const ArbiterUpdate r =
      a.update(fix(Source::Rds, 1000000, T0 + 1000000 + 800000, 200000, 2));
  AT_CHECK(r.action == Action::SlewedLarge);
}

// A large jump is withheld until a second, different source corroborates it.
AT_TEST(arb_large_corroborated_by_second_source) {
  Arbiter a;
  a.update(fix(Source::Rds, 0, T0, 200000, 1));
  const ArbiterUpdate r1 =
      a.update(fix(Source::Rds, 1000000, T0 + 1000000 + 800000, 200000, 1));
  AT_CHECK(r1.action == Action::Rejected);
  // WWV agrees about the same UTC a bit later => accepted.
  const ArbiterUpdate r2 =
      a.update(fix(Source::Wwv, 2000000, T0 + 2000000 + 800000, 30000, 1));
  AT_CHECK(r2.action == Action::SlewedLarge);
}

// Uncertainty grows with time since the last sync, and eventually unsyncs.
AT_TEST(arb_uncertainty_growth) {
  Arbiter a;
  a.update(fix(Source::Wwv, 0, T0, 20000, 1));  // ±20 ms source
  AT_CHECK(a.isSynced(0));
  AT_CHECK(a.uncertaintyUs(0) < 100000);
  // ~16.7 h later at 20 ppm growth => ~1.2 s uncertainty => unsynced.
  AT_CHECK(!a.isSynced(60000LL * 1000000));
}

// End-to-end: a crystal running 25 ppm slow is learned, and the residual
// per-fix offset collapses toward zero. Exercises clock + drift + arbiter.
AT_TEST(arb_learns_drift_closed_loop) {
  ArbiterConfig cfg;  // defaults: gain 0.5
  Arbiter a(cfg);
  const double E = 25.0;                 // ppm slow
  const int64_t hour = 3600LL * 1000000; // real µs per fix
  int64_t last_offset = 0;

  for (int k = 0; k <= 20; ++k) {
    const int64_t realt = static_cast<int64_t>(k) * hour;
    // Device monotonic runs slow by E ppm relative to real time.
    const int64_t mono =
        static_cast<int64_t>(llround(static_cast<double>(realt) * (1.0 - E / 1e6)));
    const ArbiterUpdate r =
        a.update(fix(Source::Rds, mono, T0 + realt, 200000, 2));
    last_offset = r.offset_us;
  }
  AT_CHECK_NEAR(a.ratePpm(), 25.0, 2.0);
  AT_CHECK(iabs(last_offset) < 20000);  // tracking to well under 20 ms/hour
}
