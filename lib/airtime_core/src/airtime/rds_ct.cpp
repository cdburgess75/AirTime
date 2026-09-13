#include "rds_ct.h"

namespace airtime {

// Days since 1970-01-01 for a civil (proleptic Gregorian) date.
// Howard Hinnant's algorithm — exact, no <ctime>/timezone dependencies.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;  // [0, 365]
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;       // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void mjdToCivil(int32_t mjd, int* year, int* month, int* day) {
  // Inverse conversion from the RDS standard (Annex G).
  const int yp = static_cast<int>((mjd - 15078.2) / 365.25);
  const int mp =
      static_cast<int>((mjd - 14956.1 - static_cast<int>(yp * 365.25)) / 30.6001);
  const int d =
      mjd - 14956 - static_cast<int>(yp * 365.25) - static_cast<int>(mp * 30.6001);
  const int k = (mp == 14 || mp == 15) ? 1 : 0;
  if (year) *year = yp + k + 1900;
  if (month) *month = mp - 1 - k * 12;
  if (day) *day = d;
}

int32_t civilToMjd(int year, int month, int day) {
  // MJD epoch (1858-11-17) is 40587 days before the Unix epoch.
  return static_cast<int32_t>(
      days_from_civil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day)) +
      40587);
}

bool decodeRdsClockTime(uint16_t blockA, uint16_t blockB, uint16_t blockC,
                        uint16_t blockD, RdsClockTime* out) {
  (void)blockA;  // PI code is not needed to decode CT itself.
  if (rdsGroupType(blockB) != 4 || rdsIsVersionB(blockB)) return false;

  // MJD: 17 bits = blockB[1:0] (MSBs) : blockC[15:1]
  const int32_t mjd =
      (static_cast<int32_t>(blockB & 0x0003) << 15) |
      static_cast<int32_t>(blockC >> 1);

  const int hour = ((blockC & 0x0001) << 4) | ((blockD >> 12) & 0x0F);
  const int minute = (blockD >> 6) & 0x3F;
  const int sense = (blockD >> 5) & 0x1;
  const int off = blockD & 0x1F;

  if (hour > 23 || minute > 59) return false;
  // Sanity-window the MJD so obviously-corrupt groups are rejected
  // (~1981-08 .. ~2058, comfortably around any real operating date).
  if (mjd < 45000 || mjd > 88000) return false;

  int y = 0, mo = 0, d = 0;
  mjdToCivil(mjd, &y, &mo, &d);
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;

  const int64_t days =
      days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));

  out->year = y;
  out->month = mo;
  out->day = d;
  out->hour = hour;
  out->minute = minute;
  out->offset_half_hours = sense ? -off : off;
  out->utc_epoch_s =
      days * 86400 + static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60;
  return true;
}

}  // namespace airtime
