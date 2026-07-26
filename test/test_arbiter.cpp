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

// A correction too large to slew is applied outright, not crawled in.
//
// The slew ceiling is 500 ppm, so applying an offset takes offset x 2000: a 10 s
// error would take 5.5 hours, an hour's error takes 87 DAYS. Measured on the
// device: a warm boot restored an NVS time from an hour-old power-down, RDS
// correctly asked for +3765 s, the arbiter accepted it — and 38 minutes later
// the clock was still 3764 s out, closing at its 563 ppm ceiling while
// reporting itself synced to ±110 ms. Accepting a correction has to mean
// applying it.
AT_TEST(arb_unslewable_correction_is_stepped) {
  Arbiter a;
  const int64_t kS = 1000000;
  a.update(fix(Source::Rds, 0, T0, 250000, 2));           // seed
  a.update(fix(Source::Rds, 60 * kS, T0 + 60 * kS, 250000, 2));  // sync properly

  // Now a corroborated 10-second correction.
  const int64_t mono = 120 * kS;
  const ArbiterUpdate r = a.update(fix(Source::Rds, mono, T0 + mono + 10 * kS, 250000, 2));
  AT_CHECK(r.action != Action::Rejected);
  AT_CHECK_EQ(a.utcAt(mono), T0 + mono + 10 * kS);   // applied NOW, in full
  AT_CHECK_EQ(a.pendingCorrectionUs(mono), 0);
}

// A sub-threshold correction still slews (rule 1) — clients must never see time
// jump backwards for an error small enough to walk off.
AT_TEST(arb_small_correction_still_slews) {
  Arbiter a;
  const int64_t kS = 1000000;
  a.update(fix(Source::Rds, 0, T0, 250000, 2));
  a.update(fix(Source::Rds, 60 * kS, T0 + 60 * kS, 250000, 2));

  const int64_t mono = 120 * kS;
  a.update(fix(Source::Rds, mono, T0 + mono + 300000, 30000, 1));  // 300 ms
  AT_CHECK(iabs(a.utcAt(mono) - (T0 + mono)) < 50000);  // barely moved yet
  AT_CHECK(a.pendingCorrectionUs(mono) > 100000);       // ...and says so
}

// Warm boot: what NVS remembers is a memory, not a measurement. The first real
// fix replaces it outright rather than being gated as a "large correction" —
// otherwise a device switched off overnight defends yesterday's time against
// the station telling it today's.
AT_TEST(arb_restored_memory_yields_to_first_fix) {
  Arbiter a;
  const int64_t hour = 3600LL * 1000000;
  a.restore(0, T0 - hour, hour);      // stale by an hour, honestly unsynced
  AT_CHECK(a.isSet());
  AT_CHECK(!a.isSynced(0));

  const ArbiterUpdate r = a.update(fix(Source::Rds, 1000, T0 + 1000, 250000, 1));
  AT_CHECK(r.action == Action::Seeded);
  AT_CHECK_EQ(a.utcAt(1000), T0 + 1000);   // the memory is simply replaced
  AT_CHECK(a.isSynced(1000));
  AT_CHECK_EQ(r.offset_us, hour);          // and it reports how wrong it was
}

// Two sources that disagree about PHASE must not be read as a FREQUENCY error.
//
// The clock here is perfect: no drift to find, so the honest answer is 0 ppm.
// But RDS insists on 400 ms late and WWV on the true minute, and each fix's
// offset is measured against a shared "previous fix" unless the estimator is
// careful. Differencing an RDS offset against a WWV offset yields the two
// sources' constant mutual bias divided by the seconds between them — a
// fictitious rate, and a huge one: 0.4 s over the 30 s that separates them
// reads as 13000 ppm.
//
// This is not hypothetical. In simulation it railed a correctly-learned
// +17.9 ppm to the ±100 ppm clamp on the first WWV fix and left the clock
// losing ~285 ms an hour — worse than no drift correction at all — and the
// poisoned figure is what gets written to NVS for the next boot.
AT_TEST(arb_source_bias_is_not_a_drift) {
  Arbiter a;
  const int64_t kS = 1000000, kMin = 60 * kS;
  const int64_t bias = 400000;  // RDS station 400 ms late, consistently

  a.update(fix(Source::Rds, 0, T0 + bias, 250000, 2));  // seed

  // An hour of alternating sources over a drift-free clock.
  for (int k = 1; k <= 60; ++k) {
    const int64_t mono = static_cast<int64_t>(k) * kMin;
    a.update(fix(Source::Rds, mono, T0 + mono + bias, 250000, 2));
    a.update(fix(Source::Wwv, mono + 30 * kS, T0 + mono + 30 * kS, 30000, 2));
  }

  // The crystal is perfect; anything large here is manufactured bias.
  AT_CHECK(std::fabs(a.ratePpm()) < 5.0);
}
