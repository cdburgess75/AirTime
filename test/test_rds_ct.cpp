#include <cstdint>

#include "rds_ct.h"
#include "test_framework.h"

using airtime::civilToMjd;
using airtime::decodeRdsClockTime;
using airtime::mjdToCivil;
using airtime::RdsClockTime;

namespace {

// Build the B/C/D words of a valid 4A group from fields (mirror of the decoder).
void buildCT(int32_t mjd, int hour, int minute, int sense, int off,
             uint16_t* B, uint16_t* C, uint16_t* D) {
  *B = static_cast<uint16_t>((4u << 12) |                       // group type 4
                             (0u << 11) |                       // version A
                             (static_cast<uint32_t>(mjd >> 15) & 0x3u));
  *C = static_cast<uint16_t>(((static_cast<uint32_t>(mjd) & 0x7FFFu) << 1) |
                             (static_cast<uint32_t>(hour >> 4) & 0x1u));
  *D = static_cast<uint16_t>(((static_cast<uint32_t>(hour) & 0x0Fu) << 12) |
                             ((static_cast<uint32_t>(minute) & 0x3Fu) << 6) |
                             ((static_cast<uint32_t>(sense) & 0x1u) << 5) |
                             (static_cast<uint32_t>(off) & 0x1Fu));
}

}  // namespace

// External anchor: the RDS standard's worked example is 1982-08-06 = MJD 45187.
AT_TEST(rds_mjd_anchor_1982_08_06) {
  AT_CHECK_EQ(civilToMjd(1982, 8, 6), 45187);
  int y = 0, m = 0, d = 0;
  mjdToCivil(45187, &y, &m, &d);
  AT_CHECK_EQ(y, 1982);
  AT_CHECK_EQ(m, 8);
  AT_CHECK_EQ(d, 6);
}

// Full decode of a known group: 1982-08-06 12:34 UTC, no offset.
// 1982-08-06 00:00Z is Unix 397440000; +12:34 = +45240 => 397485240.
AT_TEST(rds_decode_known_group) {
  uint16_t B, C, D;
  buildCT(45187, 12, 34, 0, 0, &B, &C, &D);
  RdsClockTime t{};
  AT_CHECK(decodeRdsClockTime(0x1234, B, C, D, &t));
  AT_CHECK_EQ(t.year, 1982);
  AT_CHECK_EQ(t.month, 8);
  AT_CHECK_EQ(t.day, 6);
  AT_CHECK_EQ(t.hour, 12);
  AT_CHECK_EQ(t.minute, 34);
  AT_CHECK_EQ(t.offset_half_hours, 0);
  AT_CHECK_EQ(t.utc_epoch_s, 397485240LL);
}

// Negative local offset (sense=1) is signed correctly (does not affect UTC).
AT_TEST(rds_decode_negative_offset) {
  uint16_t B, C, D;
  buildCT(civilToMjd(2026, 7, 25), 14, 22, 1, 12, &B, &C, &D);  // -6h (12 half-hours)
  RdsClockTime t{};
  AT_CHECK(decodeRdsClockTime(0xABCD, B, C, D, &t));
  AT_CHECK_EQ(t.year, 2026);
  AT_CHECK_EQ(t.month, 7);
  AT_CHECK_EQ(t.day, 25);
  AT_CHECK_EQ(t.hour, 14);
  AT_CHECK_EQ(t.minute, 22);
  AT_CHECK_EQ(t.offset_half_hours, -12);
}

// Round-trip a spread of dates/times through build -> decode.
AT_TEST(rds_roundtrip_various) {
  struct Row { int y, mo, d, h, mi; };
  const Row rows[] = {
      {2000, 1, 1, 0, 0},   {2024, 2, 29, 23, 59}, {2026, 12, 31, 6, 30},
      {2038, 7, 4, 12, 0},  {2035, 11, 15, 9, 45},
  };
  for (const Row& r : rows) {
    uint16_t B, C, D;
    buildCT(civilToMjd(r.y, r.mo, r.d), r.h, r.mi, 0, 0, &B, &C, &D);
    RdsClockTime t{};
    AT_CHECK(decodeRdsClockTime(0, B, C, D, &t));
    AT_CHECK_EQ(t.year, r.y);
    AT_CHECK_EQ(t.month, r.mo);
    AT_CHECK_EQ(t.day, r.d);
    AT_CHECK_EQ(t.hour, r.h);
    AT_CHECK_EQ(t.minute, r.mi);
  }
}

// Non-4A groups and out-of-range fields are rejected.
AT_TEST(rds_rejects_bad_groups) {
  uint16_t B, C, D;
  RdsClockTime t{};

  // Wrong group type (2A instead of 4A).
  buildCT(45187, 12, 0, 0, 0, &B, &C, &D);
  uint16_t B2 = static_cast<uint16_t>((B & 0x0FFF) | (2u << 12));
  AT_CHECK(!decodeRdsClockTime(0, B2, C, D, &t));

  // Version B flag set.
  uint16_t Bv = static_cast<uint16_t>(B | (1u << 11));
  AT_CHECK(!decodeRdsClockTime(0, Bv, C, D, &t));

  // Impossible hour (25).
  buildCT(45187, 25, 0, 0, 0, &B, &C, &D);
  AT_CHECK(!decodeRdsClockTime(0, B, C, D, &t));

  // Impossible minute (60).
  buildCT(45187, 10, 60, 0, 0, &B, &C, &D);
  AT_CHECK(!decodeRdsClockTime(0, B, C, D, &t));
}
