#include "airtime/dial.h"
#include "test_framework.h"

using airtime::DialBandSpan;
using airtime::pickDialBand;

namespace {

// The mode constants as the firmware defines them (Common.h). The picker
// treats them as opaque; the test uses the real values so the table below can
// be read against Menu.cpp directly.
constexpr uint8_t FM = 0, LSB = 1, USB = 2, AM = 3;

// The shipped ats-mini band table — bands[] in Menu.cpp, reduced to the fields
// the choice consumes. A copy, deliberately: this test exists to pin the
// picker's behaviour against the REAL overlaps (ALL swallowing everything,
// 41M shadowing 40M), and a synthetic table would quietly stop testing the
// trap the day it stopped resembling the truth. If Menu.cpp's table changes,
// change this with it — the indices below are asserted by band NAME in the
// comments, so a mismatch reads as what it is.
const DialBandSpan kShipped[] = {
    {6400, 10800, true, FM},    //  0 VHF
    {150, 30000, false, AM},    //  1 ALL — the 30 MHz-wide trap
    {25600, 26100, false, AM},  //  2 11M
    {21500, 21900, false, AM},  //  3 13M
    {18900, 19100, false, AM},  //  4 15M
    {17400, 18100, false, AM},  //  5 16M
    {15100, 15900, false, AM},  //  6 19M
    {13500, 13900, false, AM},  //  7 22M
    {11000, 13000, false, AM},  //  8 25M
    {9000, 11000, false, AM},   //  9 31M
    {7000, 9000, false, AM},    // 10 41M — shadows 40M by span
    {5000, 7000, false, AM},    // 11 49M
    {4000, 5100, false, AM},    // 12 60M
    {3500, 4000, false, AM},    // 13 75M
    {3000, 3500, false, AM},    // 14 90M
    {1700, 3500, false, AM},    // 15 MW3
    {495, 1701, false, AM},     // 16 MW2
    {150, 1800, false, AM},     // 17 MW1
    {1800, 2000, false, LSB},   // 18 160M
    {3500, 4000, false, LSB},   // 19 80M
    {7000, 7300, false, LSB},   // 20 40M
    {10000, 10200, false, LSB}, // 21 30M
    {14000, 14400, false, USB}, // 22 20M
    {18000, 18200, false, USB}, // 23 17M
    {21000, 21500, false, USB}, // 24 15M
    {24800, 25000, false, USB}, // 25 12M
    {28000, 29700, false, USB}, // 26 10M
    {25000, 28000, false, AM},  // 27 CB
};
constexpr std::size_t kN = sizeof(kShipped) / sizeof(kShipped[0]);

}  // namespace

// The bug as shipped: first-match returns "ALL" (index 1) for every HF
// frequency in existence. Narrowest-wins must find the band someone actually
// drew around the frequency.
AT_TEST(dial_narrowest_band_wins_over_all) {
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 7047, false, USB), 20);   // 40M
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 7200, false, LSB), 20);   // 40M
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 14300, false, USB), 22);  // 20M
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 3910, false, LSB), 19);   // 80M, tie
}

// The every-net sweep: the whole shipped net directory resolves, and never to
// the general-coverage band. This is the assertion that was once a scratch
// program pasted into a shell; a table edit that reintroduces the trap now
// fails here instead of on the air.
AT_TEST(dial_shipped_nets_never_land_on_general_coverage) {
  struct { int32_t khz; uint8_t mode; } nets[] = {
      {3862, LSB}, {3910, LSB}, {3915, LSB}, {3940, LSB}, {3965, LSB},
      {3975, LSB}, {3980, LSB}, {7242, LSB}, {7251, LSB}, {7255, LSB},
      {7258, LSB}, {7268, LSB}, {14300, USB}, {14325, USB},
  };
  for (const auto& n : nets) {
    const int b = pickDialBand(kShipped, kN, n.khz, false, n.mode);
    AT_CHECK(b >= 0);
    AT_CHECK(b != 1);                              // never "ALL"
    AT_CHECK(kShipped[b].min_khz <= n.khz && n.khz <= kShipped[b].max_khz);
    AT_CHECK(!kShipped[b].fm);
  }
}

// Ties break toward the wanted mode. 80M (LSB) and 75M (AM) are both exactly
// 3500-4000: an LSB net must land on the LSB band whichever order the table
// lists them in, so tuning it does not rewrite an AM band's mode.
AT_TEST(dial_ties_prefer_the_wanted_mode) {
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 3800, false, LSB), 19);   // 80M
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 3800, false, AM), 13);    // 75M
}

// FM and not-FM never substitute for each other, whatever the numbers say —
// 9110 (SI4735 FM units) sits numerically inside four shortwave bands.
AT_TEST(dial_fm_and_hf_are_separate_worlds) {
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 9110, true, FM), 0);      // VHF
  AT_CHECK(pickDialBand(kShipped, kN, 9110, false, AM) != 0);
}

// Out of reach is -1, not "somewhere plausible". 50140 kHz (6 m — the live
// NetLogger feed carries such nets) is above every band this chip can tune.
AT_TEST(dial_unreachable_is_refused_not_approximated) {
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 50140, false, USB), -1);
  AT_CHECK_EQ(pickDialBand(kShipped, kN, 120, false, AM), -1);
  AT_CHECK_EQ(pickDialBand(nullptr, 0, 7047, false, USB), -1);
}
