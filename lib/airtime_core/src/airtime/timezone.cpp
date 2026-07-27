#include "timezone.h"

#include "rds_ct.h"  // mjdToCivil / civilToMjd

namespace airtime {

const TimeZoneRule kZoneEastern  = {"EST",  "EDT",  -5 * 3600};
const TimeZoneRule kZoneCentral  = {"CST",  "CDT",  -6 * 3600};
const TimeZoneRule kZoneMountain = {"MST",  "MDT",  -7 * 3600};
const TimeZoneRule kZoneArizona  = {"MST",  nullptr, -7 * 3600};
const TimeZoneRule kZonePacific  = {"PST",  "PDT",  -8 * 3600};
const TimeZoneRule kZoneAlaska   = {"AKST", "AKDT", -9 * 3600};
const TimeZoneRule kZoneHawaii   = {"HST",  nullptr, -10 * 3600};
const TimeZoneRule kZoneUtc      = {"UTC",  nullptr, 0};

namespace {

// Floor division: C's / truncates toward zero, which puts pre-1970 or
// west-of-Greenwich instants in the wrong day.
int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

constexpr int64_t kMjdUnixEpoch = 40587;  // MJD of 1970-01-01

// MJD of the nth Sunday of a month (nth is 1-based).
//
// MJD 0 is 1858-11-17, a Wednesday, so (mjd + 3) % 7 numbers the days with
// Sunday == 0.
int32_t nthSundayMjd(int year, int month, int nth) {
  const int32_t first = civilToMjd(year, month, 1);
  const int dow = static_cast<int>(((first % 7) + 7 + 3) % 7);
  const int32_t first_sunday = first + ((7 - dow) % 7);
  return first_sunday + 7 * (nth - 1);
}

int64_t mjdToUnixS(int32_t mjd) {
  return (static_cast<int64_t>(mjd) - kMjdUnixEpoch) * 86400;
}

}  // namespace

bool usDstInEffect(int64_t utc_s, int32_t std_offset_s) {
  // Which year is it where the operator is standing? Using UTC's year would
  // pick the wrong transition dates for a few hours either side of New Year.
  const int64_t local_std = utc_s + std_offset_s;
  int year = 0, month = 0, day = 0;
  mjdToCivil(static_cast<int32_t>(floorDiv(local_std, 86400) + kMjdUnixEpoch),
             &year, &month, &day);

  // Spring forward: 02:00 local STANDARD, second Sunday in March.
  const int64_t start_utc =
      mjdToUnixS(nthSundayMjd(year, 3, 2)) + 2 * 3600 - std_offset_s;
  // Fall back: 02:00 local DAYLIGHT (= 01:00 standard), first Sunday in
  // November.
  const int64_t end_utc =
      mjdToUnixS(nthSundayMjd(year, 11, 1)) + 1 * 3600 - std_offset_s;

  return utc_s >= start_utc && utc_s < end_utc;
}

int64_t localEpochS(const TimeZoneRule& tz, int64_t utc_s, const char** name_out) {
  const bool dst =
      tz.dst_name != nullptr && usDstInEffect(utc_s, tz.std_offset_s);
  if (name_out != nullptr) *name_out = dst ? tz.dst_name : tz.std_name;
  return utc_s + tz.std_offset_s + (dst ? 3600 : 0);
}

}  // namespace airtime
