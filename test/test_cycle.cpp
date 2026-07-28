#include <cstdint>

#include "airtime/cycle.h"
#include "test_framework.h"

using namespace airtime;

namespace {
constexpr int64_t kSec = 1000000;
constexpr int64_t kFt8 = 15 * kSec;
constexpr int64_t kFt4 = 7500000;   // 7.5 s — the one fractional period
constexpr int64_t kWspr = 120 * kSec;
}  // namespace

AT_TEST(cycle_opens_on_the_boundary) {
  // Exactly on a slot edge: nothing elapsed, a whole slot remaining. The bar
  // must read empty here rather than full, or every boundary shows the
  // PREVIOUS slot completing for one frame.
  const CyclePhase p = cyclePhaseAt(100 * kFt8, kFt8);
  AT_CHECK_EQ(p.into_us, 0);
  AT_CHECK_EQ(p.remain_us, kFt8);
  AT_CHECK_EQ(p.index, 100);
  AT_CHECK_NEAR(p.fraction, 0.0f, 1e-6f);
}

AT_TEST(cycle_counts_down_to_the_next_boundary) {
  const CyclePhase p = cyclePhaseAt(100 * kFt8 + 3 * kSec + 470000, kFt8);
  AT_CHECK_EQ(p.into_us, 3470000);
  AT_CHECK_EQ(p.remain_us, kFt8 - 3470000);
  AT_CHECK_EQ(p.index, 100);
}

AT_TEST(cycle_never_reports_a_full_bar) {
  // One microsecond before the edge is still THIS slot, with 1 us left. A
  // remain_us of zero would mean the countdown displays "T-0.00" for a frame,
  // which reads as a stalled clock on the very instrument meant to prove the
  // clock is not stalled.
  const CyclePhase p = cyclePhaseAt(101 * kFt8 - 1, kFt8);
  AT_CHECK_EQ(p.remain_us, 1);
  AT_CHECK_EQ(p.index, 100);
  AT_CHECK(p.fraction < 1.0f);
}

AT_TEST(cycle_ft8_slots_land_on_quarter_minutes) {
  // FT8 boundaries are :00 :15 :30 :45 of every UTC minute. Walk a minute and
  // assert the edges fall exactly there — this is the property an operator
  // checks against WSJT-X, so it is the property worth pinning.
  for (int64_t minute = 0; minute < 4; ++minute) {
    for (int q = 0; q < 4; ++q) {
      const int64_t t = minute * 60 * kSec + q * kFt8;
      AT_CHECK_EQ(cyclePhaseAt(t, kFt8).into_us, 0);
      // ...and one microsecond earlier is emphatically NOT a boundary.
      AT_CHECK(cyclePhaseAt(t - 1, kFt8).into_us != 0);
    }
  }
}

AT_TEST(cycle_ft4_handles_the_half_second_period) {
  // 7.5 s does not divide a second, which is why the arithmetic is in
  // microseconds. Boundaries alternate on and off the whole second.
  AT_CHECK_EQ(cyclePhaseAt(0, kFt4).into_us, 0);
  AT_CHECK_EQ(cyclePhaseAt(7500000, kFt4).into_us, 0);
  AT_CHECK_EQ(cyclePhaseAt(15 * kSec, kFt4).into_us, 0);
  AT_CHECK_EQ(cyclePhaseAt(22500000, kFt4).into_us, 0);
  // Mid-slot, on a whole second that is NOT an edge.
  AT_CHECK_EQ(cyclePhaseAt(4 * kSec, kFt4).into_us, 4 * kSec);
}

AT_TEST(cycle_wspr_starts_on_even_minutes) {
  // WSPR transmits in the 2-minute window that begins on an EVEN minute. The
  // Unix epoch begins on minute 0, which is even, so plain modular arithmetic
  // gets this right for free — but only by luck of the epoch's definition, so
  // it is asserted rather than assumed.
  for (int64_t m = 0; m < 10; ++m) {
    const int64_t t = m * 60 * kSec;
    const bool boundary = cyclePhaseAt(t, kWspr).into_us == 0;
    AT_CHECK_EQ(boundary, (m % 2) == 0);
  }
}

AT_TEST(cycle_slot_parity_alternates) {
  // FT8 alternates TX and RX slots; the panel colours the bar by parity so the
  // alternation is visible at a glance. Consecutive slots must disagree.
  bool prev = cyclePhaseAt(0, kFt8).odd_slot;
  for (int i = 1; i < 8; ++i) {
    const bool now = cyclePhaseAt(i * kFt8, kFt8).odd_slot;
    AT_CHECK(now != prev);
    prev = now;
  }
}

AT_TEST(cycle_survives_a_clock_before_the_epoch) {
  // The disciplined clock starts at zero and can be restored from a corrupt
  // blob, so a negative instant is reachable. Truncating division would push
  // the boundary the wrong side of the moment and hand back a negative
  // into_us — a bar that fills backwards. Floor division is what keeps every
  // field non-negative.
  const CyclePhase p = cyclePhaseAt(-1, kFt8);
  AT_CHECK_EQ(p.into_us, kFt8 - 1);
  AT_CHECK_EQ(p.remain_us, 1);
  AT_CHECK_EQ(p.index, -1);
  AT_CHECK(p.fraction >= 0.0f && p.fraction < 1.0f);

  const CyclePhase q = cyclePhaseAt(-kFt8, kFt8);
  AT_CHECK_EQ(q.into_us, 0);
  AT_CHECK_EQ(q.index, -1);
}

AT_TEST(cycle_rejects_an_unusable_period) {
  // A zero period must not divide by zero. Returns an inert phase; the panel
  // draws an empty bar rather than taking the device down.
  const CyclePhase p = cyclePhaseAt(12345678, 0);
  AT_CHECK_EQ(p.into_us, 0);
  AT_CHECK_EQ(p.remain_us, 0);
  AT_CHECK_NEAR(p.fraction, 0.0f, 1e-6f);
}

AT_TEST(cycle_mode_table_is_sane) {
  AT_CHECK(kCycleModeCount >= 4);
  for (std::size_t i = 0; i < kCycleModeCount; ++i) {
    AT_CHECK(kCycleModes[i].name != nullptr);
    AT_CHECK(kCycleModes[i].name[0] != '\0');
    AT_CHECK(kCycleModes[i].period_us > 0);
    // Every published slot length divides a minute evenly (or is a whole
    // number of them). That is the property that lets modular arithmetic off
    // the epoch be correct, so a future addition that breaks it fails here
    // rather than drifting quietly against the band.
    const int64_t minute = 60 * kSec;
    const bool divides = (minute % kCycleModes[i].period_us) == 0;
    const bool whole_minutes = (kCycleModes[i].period_us % minute) == 0;
    AT_CHECK(divides || whole_minutes);
  }
  AT_CHECK(std::string(kCycleModes[0].name) == "FT8");
}
