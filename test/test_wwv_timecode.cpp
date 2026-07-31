#include <cstdint>
#include <vector>

#include "airtime/wwv_timecode.h"
#include "test_framework.h"

using namespace airtime;

namespace {

const WwvTimecodeConfig kCfg;

// A synthetic WWV transmitter, built from the SAME table the decoder reads.
//
// That shared table is a deliberate limitation and worth being honest about:
// these tests prove the decoder inverts the encoder — framing, corroboration,
// ranges, leap years, the century rule — but they CANNOT prove the bit map
// matches Fort Collins. Only air can do that, which is why the decoder exposes
// its raw frame. What is proved here is everything that would still be broken
// if the map were right, and that is most of the surface.
struct Encoder {
  TcSymbol sym[60];

  void clear() {
    for (int i = 0; i < 60; ++i) sym[i] = TcSymbol::Zero;
    const int markers[] = {9, 19, 29, 39, 49, 59};
    for (int m : markers) sym[m] = TcSymbol::Marker;
    sym[0] = TcSymbol::Marker;   // frame reference
  }

  // Set a BCD group by (second, weight) pairs, greedily from the top. `value`
  // is in the SAME units as the weights — so a tens group of {10,20,40} takes
  // 30, not 3. Getting that wrong is how the first version of this encoder
  // silently dropped every tens digit and made the decoder look broken.
  void field(const int (*pairs)[2], int n, int value) {
    int remaining = value;
    for (int i = n - 1; i >= 0; --i) {
      if (remaining >= pairs[i][1]) {
        remaining -= pairs[i][1];
        sym[pairs[i][0]] = TcSymbol::One;
      }
    }
  }

  void build(int minute, int hour, int doy, int yy) {
    clear();
    const int mins[][2] = {{1,1},{2,2},{3,4},{4,8},{6,10},{7,20},{8,40}};
    const int hrs[][2]  = {{12,1},{13,2},{14,4},{15,8},{16,10},{17,20}};
    const int day[][2]  = {{22,1},{23,2},{24,4},{25,8},
                           {26,10},{27,20},{28,40},{30,80},{31,100},{32,200}};
    const int yr[][2]   = {{45,1},{46,2},{47,4},{48,8},
                           {50,10},{51,20},{52,40},{53,80}};
    // BCD, not plain binary: units and tens are separate nibbles, so split the
    // value first and fill each group greedily.
    field(mins, 4, minute % 10);  field(mins + 4, 3, (minute / 10) * 10);
    field(hrs, 4, hour % 10);     field(hrs + 4, 2, (hour / 10) * 10);
    field(day, 4, doy % 10);      field(day + 4, 4, ((doy / 10) % 10) * 10);
    field(day + 8, 2, (doy / 100) * 100);
    field(yr, 4, yy % 10);        field(yr + 4, 4, (yy / 10) * 10);
  }

  // Pulse length in ms for each symbol, as the air would carry it.
  int64_t ms(int second) const {
    switch (sym[second]) {
      case TcSymbol::Zero:   return kCfg.zero_ms;
      case TcSymbol::One:    return kCfg.one_ms;
      case TcSymbol::Marker: return kCfg.marker_ms;
      default:               return 0;
    }
  }
};

// Drive a decoder through `minutes` consecutive frames starting at the given
// time, returning how many were confirmed.
int runMinutes(WwvTimecodeDecoder& d, int minute, int hour, int doy, int yy,
               int minutes, int64_t t0 = 0) {
  int confirmed = 0;
  int64_t t = t0;
  for (int m = 0; m < minutes; ++m) {
    Encoder e;
    e.build(minute, hour, doy, yy);
    for (int s = 0; s < 60; ++s) {
      if (d.onSecond(e.ms(s), t)) ++confirmed;
      t += 1000000;
    }
    if (++minute == 60) { minute = 0; if (++hour == 24) { hour = 0; ++doy; } }
  }
  return confirmed;
}

}  // namespace

AT_TEST(wwvtc_classifies_the_three_pulse_widths) {
  AT_CHECK(classifyPulse(170, kCfg) == TcSymbol::Zero);
  AT_CHECK(classifyPulse(470, kCfg) == TcSymbol::One);
  AT_CHECK(classifyPulse(770, kCfg) == TcSymbol::Marker);
  // The nominal widths are 300 ms apart, so generous tolerance still cannot
  // let one symbol read as its neighbour — the property that makes a wide
  // acceptance window safe on a fading HF path.
  AT_CHECK(classifyPulse(170 + 89, kCfg) == TcSymbol::Zero);
  AT_CHECK(classifyPulse(470 - 89, kCfg) == TcSymbol::One);
  AT_CHECK(classifyPulse(320, kCfg) == TcSymbol::Invalid);   // between 0 and 1
  AT_CHECK(classifyPulse(0, kCfg) == TcSymbol::Invalid);
  AT_CHECK(classifyPulse(2000, kCfg) == TcSymbol::Invalid);
}

AT_TEST(wwvtc_decodes_a_well_formed_frame) {
  Encoder e;
  e.build(/*minute=*/37, /*hour=*/14, /*doy=*/209, /*yy=*/26);
  const WwvTime t = decodeFrame(e.sym, 60, 2026);
  AT_CHECK(t.valid);
  AT_CHECK_EQ(t.minute, 37);
  AT_CHECK_EQ(t.hour, 14);
  AT_CHECK_EQ(t.day_of_year, 209);
  AT_CHECK_EQ(t.year, 2026);
}

AT_TEST(wwvtc_rejects_a_frame_with_an_unreadable_pulse) {
  // One hole is enough. A frame with a guessed digit in it is worse than no
  // frame: the next one is only sixty seconds away.
  Encoder e;
  e.build(12, 6, 100, 26);
  e.sym[23] = TcSymbol::Invalid;
  AT_CHECK(!decodeFrame(e.sym, 60, 2026).valid);
}

AT_TEST(wwvtc_rejects_misplaced_position_markers) {
  // A marker where none belongs means the alignment is wrong, whatever the
  // rest of the frame happens to decode to. Both directions are checked:
  // a missing marker and a spurious one.
  Encoder e;
  e.build(12, 6, 100, 26);
  e.sym[19] = TcSymbol::Zero;              // marker removed
  AT_CHECK(!decodeFrame(e.sym, 60, 2026).valid);

  e.build(12, 6, 100, 26);
  e.sym[34] = TcSymbol::Marker;            // marker invented
  AT_CHECK(!decodeFrame(e.sym, 60, 2026).valid);
}

AT_TEST(wwvtc_rejects_impossible_dates) {
  // BCD can carry values a calendar cannot. Day 366 in a common year is the
  // sharp case: structurally perfect, chronologically impossible.
  Encoder e;
  e.build(0, 0, 366, 26);                   // 2026 is not a leap year
  AT_CHECK(!decodeFrame(e.sym, 60, 2026).valid);

  e.build(0, 0, 366, 24);                   // 2024 is
  AT_CHECK(decodeFrame(e.sym, 60, 2024).valid);
}

AT_TEST(wwvtc_needs_two_consecutive_frames_before_it_believes_itself) {
  // The rule that stands between a wrong bit map and a wrong clock. One frame
  // is never enough, however clean.
  WwvTimecodeDecoder d(kCfg, 2026);
  const int confirmed = runMinutes(d, 30, 12, 209, 26, /*minutes=*/1);
  AT_CHECK_EQ(confirmed, 0);
  AT_CHECK(!d.haveFrame());

  // Frame hunting costs the first minute (it aligns on the 59/0 marker pair),
  // decoding the second, and confirming the third.
  WwvTimecodeDecoder d2(kCfg, 2026);
  AT_CHECK(runMinutes(d2, 30, 12, 209, 26, /*minutes=*/4) >= 1);
  AT_CHECK(d2.haveFrame());
  AT_CHECK_EQ(d2.time().hour, 12);
}

AT_TEST(wwvtc_refuses_frames_that_do_not_advance_by_one_minute) {
  // Feed the SAME minute over and over — the shape a stuck or mis-aligned
  // decoder produces. Structurally each frame is perfect; none may ever be
  // confirmed, because a real minute always increments.
  WwvTimecodeDecoder d(kCfg, 2026);
  int confirmed = 0;
  int64_t t = 0;
  for (int m = 0; m < 6; ++m) {
    Encoder e;
    e.build(30, 12, 209, 26);              // never advances
    for (int s = 0; s < 60; ++s) {
      if (d.onSecond(e.ms(s), t)) ++confirmed;
      t += 1000000;
    }
  }
  AT_CHECK_EQ(confirmed, 0);
  AT_CHECK(!d.haveFrame());
}

AT_TEST(wwvtc_survives_the_hour_and_day_rollover) {
  // 23:59 -> 00:00 advances the day, which the corroboration check has to see
  // as exactly sixty seconds. Epoch arithmetic, not field arithmetic, is what
  // makes that work.
  WwvTimecodeDecoder d(kCfg, 2026);
  AT_CHECK(runMinutes(d, 58, 23, 209, 26, /*minutes=*/5) >= 1);
  AT_CHECK(d.haveFrame());
  // Landed in the next day.
  AT_CHECK_EQ(d.time().day_of_year, 210);
  AT_CHECK_EQ(d.time().hour, 0);
}

AT_TEST(wwvtc_epoch_conversion_handles_leap_years) {
  WwvTime t;
  t.valid = true;
  t.year = 2026; t.day_of_year = 1; t.hour = 0; t.minute = 0;
  // 2026-01-01T00:00:00Z
  AT_CHECK_EQ(wwvTimeToEpochS(t), 1767225600);

  // Day 60 of a leap year is 29 February; of a common year, 1 March. Getting
  // this wrong would put the clock a day out for ten months of the year.
  WwvTime leap = t;
  leap.year = 2024; leap.day_of_year = 60;
  const int64_t feb29 = wwvTimeToEpochS(leap);   // 2024-02-29T00:00Z
  AT_CHECK_EQ(feb29, 1709164800);

  WwvTime common = t;
  common.year = 2026; common.day_of_year = 60;   // 2026-03-01T00:00Z
  AT_CHECK_EQ(wwvTimeToEpochS(common), 1772323200);
}

AT_TEST(wwvtc_two_digit_year_resolves_to_the_nearest_century) {
  // WWV sends "26". Something has to supply the century and there is no other
  // source by definition, so the build year anchors it.
  Encoder e;
  e.build(0, 0, 1, 26);
  AT_CHECK_EQ(decodeFrame(e.sym, 60, 2026).year, 2026);

  // A device still running in 2098 hearing "02" should read 2102, not 2002.
  e.build(0, 0, 1, 2);
  AT_CHECK_EQ(decodeFrame(e.sym, 60, 2098).year, 2102);

  // ...and one in 2101 hearing "98" should read 2098, not 2198.
  e.build(0, 0, 1, 98);
  AT_CHECK_EQ(decodeFrame(e.sym, 60, 2101).year, 2098);
}

AT_TEST(wwvtc_recovers_after_losing_the_signal) {
  // A fade mid-frame must not leave the decoder permanently confused. It drops
  // back to hunting and picks the code up again on the next clean pair of
  // minutes — the normal condition on HF, not an exception.
  WwvTimecodeDecoder d(kCfg, 2026);
  runMinutes(d, 30, 12, 209, 26, 3);
  AT_CHECK(d.haveFrame());

  int64_t t = 100000000;
  for (int s = 0; s < 25; ++s) { d.onSecond(0, t); t += 1000000; }  // dead air

  const int again = runMinutes(d, 10, 15, 209, 26, 4, t);
  AT_CHECK(again >= 1);
  AT_CHECK_EQ(d.time().hour, 15);
}

AT_TEST(wwvtc_reports_dut1_and_flags) {
  Encoder e;
  e.build(0, 0, 1, 26);
  e.sym[38] = TcSymbol::One;    // DUT1 positive
  e.sym[40] = TcSymbol::One;    // +100 ms
  e.sym[41] = TcSymbol::One;    // +200 ms
  e.sym[55] = TcSymbol::One;    // leap warning
  e.sym[58] = TcSymbol::One;    // DST now
  const WwvTime t = decodeFrame(e.sym, 60, 2026);
  AT_CHECK(t.valid);
  AT_CHECK_EQ(t.dut1_ms, 300);
  AT_CHECK(t.leap_warning);
  AT_CHECK(t.dst_now);

  e.sym[38] = TcSymbol::Zero;   // sign negative
  AT_CHECK_EQ(decodeFrame(e.sym, 60, 2026).dut1_ms, -300);
}
