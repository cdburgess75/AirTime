#include "wwv_timecode.h"

#include "rds_ct.h"   // civilToMjd / mjdToCivil — one calendar in this codebase

namespace airtime {
namespace {

// ── The bit map ─────────────────────────────────────────────────────────────
// Second index -> BCD weight. Everything not listed is unused by us (WWV sends
// zeros, or fields this device has no use for). Position markers are asserted
// separately; see kMarkerSeconds.
//
// DATA, not logic: if a capture from the air disagrees, one number changes here
// and nothing else in the file moves.
struct BitWeight { uint8_t second; int weight; };

const BitWeight kMinuteBits[] = {
    {1, 1}, {2, 2}, {3, 4}, {4, 8},          // units
    {6, 10}, {7, 20}, {8, 40},               // tens
};
const BitWeight kHourBits[] = {
    {12, 1}, {13, 2}, {14, 4}, {15, 8},      // units
    {16, 10}, {17, 20},                      // tens
};
const BitWeight kDoyBits[] = {
    {22, 1}, {23, 2}, {24, 4}, {25, 8},      // units
    {26, 10}, {27, 20}, {28, 40}, {30, 80},  // tens (30 is past P3)
    {31, 100}, {32, 200},                    // hundreds
};
const BitWeight kYearBits[] = {
    {45, 1}, {46, 2}, {47, 4}, {48, 8},      // units
    {50, 10}, {51, 20}, {52, 40}, {53, 80},  // tens (past P5)
};
const BitWeight kDut1Bits[] = {
    {40, 100}, {41, 200}, {42, 400}, {43, 800},   // milliseconds
};
constexpr uint8_t kDut1SignSecond = 38;   // 1 = positive
constexpr uint8_t kLeapWarnSecond = 55;
constexpr uint8_t kDstNowSecond = 58;
constexpr uint8_t kDstSoonSecond = 57;

// Where position markers must be. Second 0 is the frame reference; the decoder
// aligns on the 59/0 marker pair, so 0 is validated by construction.
const uint8_t kMarkerSeconds[] = {9, 19, 29, 39, 49, 59};

template <std::size_t N>
int bcd(const TcSymbol (&sym)[60], const BitWeight (&bits)[N]) {
  int v = 0;
  for (std::size_t i = 0; i < N; ++i)
    if (sym[bits[i].second] == TcSymbol::One) v += bits[i].weight;
  return v;
}

// The inverse of bcd(): scatter a value across its weighted seconds. Greedy
// from the largest weight, which is exact for these tables because each is a
// BCD digit set (weights within a digit sum to less than the next digit up).
template <std::size_t N>
void unbcd(int v, TcSymbol (&sym)[60], const BitWeight (&bits)[N]) {
  for (std::size_t i = N; i > 0; --i) {
    const BitWeight& b = bits[i - 1];
    if (v >= b.weight) {
      sym[b.second] = TcSymbol::One;
      v -= b.weight;
    }
  }
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// The seven seconds that define the frame's shape: 0 plus the six position
// markers. If all seven read Marker, the alignment cannot be wrong — the
// chance of seven noise bursts landing in exactly those cells is not a thing
// a fading path produces.
bool markerSkeletonIntact(const TcSymbol* s) {
  if (s[0] != TcSymbol::Marker) return false;
  for (std::size_t i = 0; i < sizeof(kMarkerSeconds); ++i)
    if (s[kMarkerSeconds[i]] != TcSymbol::Marker) return false;
  return true;
}

}  // namespace

TcSymbol classifyPulse(int64_t pulse_ms, const WwvTimecodeConfig& cfg) {
  const int64_t t = cfg.tolerance_ms;
  if (pulse_ms >= cfg.zero_ms - t && pulse_ms <= cfg.zero_ms + t)
    return TcSymbol::Zero;
  if (pulse_ms >= cfg.one_ms - t && pulse_ms <= cfg.one_ms + t)
    return TcSymbol::One;
  if (pulse_ms >= cfg.marker_ms - t && pulse_ms <= cfg.marker_ms + t)
    return TcSymbol::Marker;
  return TcSymbol::Invalid;
}

WwvTime decodeFrame(const TcSymbol* sym, std::size_t n, int century_hint_year) {
  WwvTime out;
  if (sym == nullptr || n < 60) return out;

  // A local copy of fixed extent, so the bcd() helper can take a reference to
  // an array of known size and the compiler checks every index against 60.
  TcSymbol s[60];
  for (std::size_t i = 0; i < 60; ++i) s[i] = sym[i];

  // Every symbol must have been recognisable. One Invalid means a pulse we
  // could not measure, and a frame with a hole in it is not a frame — better
  // to wait a minute than to publish a time with a guessed digit in it.
  for (std::size_t i = 0; i < 60; ++i)
    if (s[i] == TcSymbol::Invalid) return out;

  // Position markers where the format says they are — second 0 included, which
  // the original hunt guaranteed by construction but a frame that FOLLOWED a
  // kept alignment does not. This is the cheapest and strongest structural
  // check available: seven seconds that must all be the long pulse, and
  // nothing else may be.
  if (!markerSkeletonIntact(s)) return out;
  for (std::size_t i = 1; i < 60; ++i) {
    bool expected = false;
    for (std::size_t k = 0; k < sizeof(kMarkerSeconds); ++k)
      if (kMarkerSeconds[k] == i) expected = true;
    if (!expected && s[i] == TcSymbol::Marker) return out;
  }

  out.minute = bcd(s, kMinuteBits);
  out.hour = bcd(s, kHourBits);
  out.day_of_year = bcd(s, kDoyBits);
  const int yy = bcd(s, kYearBits);

  // WWV sends two digits. Something must supply the century and there is no
  // second source here by definition — so anchor on the build year and pick
  // the nearest candidate. Wrong only if the device is used more than 50 years
  // from now, which is a failure mode worth accepting.
  const int base = century_hint_year - (century_hint_year % 100);
  out.year = base + yy;
  if (out.year - century_hint_year > 50) out.year -= 100;
  if (century_hint_year - out.year > 50) out.year += 100;

  out.dst_now = s[kDstNowSecond] == TcSymbol::One;
  out.dst_soon = s[kDstSoonSecond] == TcSymbol::One;
  out.leap_warning = s[kLeapWarnSecond] == TcSymbol::One;
  out.dut1_ms = bcd(s, kDut1Bits);
  if (s[kDut1SignSecond] != TcSymbol::One) out.dut1_ms = -out.dut1_ms;

  // Ranges. A BCD field can encode values the calendar has no use for — 0x0F
  // in an hours field is 15, but nibbles that should never exceed 9 can carry
  // 13, and a day-of-year of 400 is not a date.
  if (out.minute > 59) return out;
  if (out.hour > 23) return out;
  if (out.day_of_year < 1 || out.day_of_year > 366) return out;
  if (out.day_of_year == 366 && !isLeap(out.year)) return out;

  out.valid = true;
  return out;
}

int64_t wwvTimeToEpochS(const WwvTime& t) {
  if (!t.valid) return 0;
  // Day-of-year through the civil calendar, so 1 March in a leap year lands
  // where it should. Jan 1 is day 1, hence the -1.
  const int32_t mjd_jan1 = civilToMjd(t.year, 1, 1);
  const int64_t days = (int64_t)mjd_jan1 - 40587 + (t.day_of_year - 1);
  return days * 86400 + (int64_t)t.hour * 3600 + (int64_t)t.minute * 60;
}

WwvTime wwvTimeFromEpochS(int64_t epoch_s) {
  WwvTime t;
  int64_t days = epoch_s / 86400;
  int64_t sod = epoch_s - days * 86400;
  if (sod < 0) { sod += 86400; --days; }
  int mo = 0, dy = 0;
  mjdToCivil((int32_t)(days + 40587), &t.year, &mo, &dy);
  t.day_of_year =
      (int)(days + 40587 - civilToMjd(t.year, 1, 1)) + 1;  // Jan 1 is day 1
  t.hour = (int)(sod / 3600);
  t.minute = (int)((sod % 3600) / 60);
  t.valid = true;
  return t;
}

void encodeFrame(const WwvTime& t, TcSymbol out[60]) {
  TcSymbol s[60];
  for (std::size_t i = 0; i < 60; ++i) s[i] = TcSymbol::Zero;
  s[0] = TcSymbol::Marker;
  for (std::size_t i = 0; i < sizeof(kMarkerSeconds); ++i)
    s[kMarkerSeconds[i]] = TcSymbol::Marker;

  unbcd(t.minute, s, kMinuteBits);
  unbcd(t.hour, s, kHourBits);
  unbcd(t.day_of_year, s, kDoyBits);
  unbcd(((t.year % 100) + 100) % 100, s, kYearBits);
  int dut = t.dut1_ms;
  if (dut >= 0) s[kDut1SignSecond] = TcSymbol::One;
  else dut = -dut;
  unbcd(dut, s, kDut1Bits);
  if (t.dst_now) s[kDstNowSecond] = TcSymbol::One;
  if (t.dst_soon) s[kDstSoonSecond] = TcSymbol::One;
  if (t.leap_warning) s[kLeapWarnSecond] = TcSymbol::One;

  for (std::size_t i = 0; i < 60; ++i) out[i] = s[i];
}

void WwvTimecodeDecoder::reset() {
  fill_ = 0;
  framed_ = false;
  last_was_marker_ = false;  // a pair must not straddle a retune
  pending_ = WwvTime();
  confirmed_ = WwvTime();
  frame_start_us_ = pending_start_us_ = confirmed_start_us_ = 0;
}

bool WwvTimecodeDecoder::onSecond(int64_t pulse_ms, int64_t second_start_us) {
  const TcSymbol s = classifyPulse(pulse_ms, cfg_);

  if (!framed_) {
    // Hunting for the frame. A marker at second 59 is followed immediately by
    // the marker at second 0 of the next minute — that adjacent PAIR is the
    // only place in the code where two markers fall in consecutive seconds, so
    // it identifies second 0 unambiguously and without knowing the time.
    if (s == TcSymbol::Marker && last_was_marker_) {
      framed_ = true;
      fill_ = 0;
      frame_start_us_ = second_start_us;
      frame_[fill_++] = s;   // this IS second 0
    }
    last_was_marker_ = (s == TcSymbol::Marker);
    return false;
  }

  last_was_marker_ = (s == TcSymbol::Marker);
  frame_[fill_++] = s;
  if (fill_ < kFrameBits) return false;

  // A whole minute is in hand. Snapshot it for rawFrame() before anything can
  // overwrite it — the status page polls on its own schedule.
  fill_ = 0;
  ++frames_seen_;
  for (std::size_t i = 0; i < kFrameBits; ++i) last_frame_[i] = frame_[i];
  const int64_t start_us = frame_start_us_;
  frame_start_us_ = second_start_us + 1000000;   // next frame opens next second
  const WwvTime got = decodeFrame(frame_, kFrameBits, century_hint_);

  if (!got.valid) {
    // An intact marker skeleton means the alignment is certainly right and
    // only the data was unreadable (fading, the voice announcement) — keep
    // the frame position and the pending candidate, and let the next minute
    // try. A broken skeleton means the alignment itself is suspect: re-hunt.
    if (!markerSkeletonIntact(frame_)) {
      framed_ = false;
      pending_ = WwvTime();
    }
    return false;
  }
  ++frames_decoded_;

  // ── Corroboration ─────────────────────────────────────────────────────────
  // Nothing is believed on one frame's evidence. A second frame must read an
  // exact whole number of minutes later — epoch arithmetic a mis-alignment or
  // a wrong bit weight cannot fake twice running. Up to three minutes, so the
  // frame a fade or the voice announcement ruined does not force the two
  // clean frames either side of it to start over.
  if (pending_.valid) {
    const int64_t a = wwvTimeToEpochS(pending_);
    const int64_t b = wwvTimeToEpochS(got);
    const int64_t d = b - a;
    if (d >= 60 && d <= 180 && d % 60 == 0) {
      confirmed_ = got;
      confirmed_start_us_ = start_us;
      ++frames_confirmed_;
      pending_ = got;
      pending_start_us_ = start_us;
      return true;
    }
  }

  pending_ = got;
  pending_start_us_ = start_us;
  return false;
}

}  // namespace airtime
