#include <cstdint>
#include <cstring>

#include "airtime/rds_ct.h"
#include "airtime/timezone.h"
#include "test_framework.h"

using airtime::civilToMjd;
using airtime::kZoneArizona;
using airtime::kZoneCentral;
using airtime::kZoneEastern;
using airtime::kZoneUtc;
using airtime::localEpochS;
using airtime::TimeZoneRule;

namespace {
// UTC seconds for a given calendar instant.
int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  return (static_cast<int64_t>(civilToMjd(y, mo, d)) - 40587) * 86400 +
         h * 3600 + mi * 60 + s;
}
bool named(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
}  // namespace

// The case that prompted this: the owner is in Central time, and on the day the
// display was built his laptop said CDT while the clock would have said CST.
// Both names are right — six months apart.
AT_TEST(tz_central_names_itself_correctly_in_both_seasons) {
  const char* name = nullptr;

  // 2026-07-26 12:00 UTC — high summer.
  int64_t local = localEpochS(kZoneCentral, utc(2026, 7, 26, 12), &name);
  AT_CHECK(named(name, "CDT"));
  AT_CHECK_EQ(local, utc(2026, 7, 26, 12) - 5 * 3600);

  // 2026-01-15 12:00 UTC — deep winter.
  local = localEpochS(kZoneCentral, utc(2026, 1, 15, 12), &name);
  AT_CHECK(named(name, "CST"));
  AT_CHECK_EQ(local, utc(2026, 1, 15, 12) - 6 * 3600);
}

// US DST 2026 runs March 8 to November 1. The transitions are defined in LOCAL
// time, so a rule that only got the day right would still be an hour wrong for
// part of the changeover day.
AT_TEST(tz_us_transitions_are_correct_to_the_hour) {
  const char* name = nullptr;

  // Spring forward at 02:00 local standard = 08:00 UTC for Central.
  localEpochS(kZoneCentral, utc(2026, 3, 8, 7, 59), &name);
  AT_CHECK(named(name, "CST"));
  localEpochS(kZoneCentral, utc(2026, 3, 8, 8, 0), &name);
  AT_CHECK(named(name, "CDT"));

  // Fall back at 02:00 local daylight = 01:00 standard = 07:00 UTC.
  localEpochS(kZoneCentral, utc(2026, 11, 1, 6, 59), &name);
  AT_CHECK(named(name, "CDT"));
  localEpochS(kZoneCentral, utc(2026, 11, 1, 7, 0), &name);
  AT_CHECK(named(name, "CST"));
}

// Eastern is an hour ahead of Central and switches at the same local hour,
// which means an hour earlier in UTC. A shared-constant bug would show here.
AT_TEST(tz_eastern_tracks_its_own_local_hour) {
  const char* name = nullptr;
  localEpochS(kZoneEastern, utc(2026, 3, 8, 6, 59), &name);
  AT_CHECK(named(name, "EST"));
  localEpochS(kZoneEastern, utc(2026, 3, 8, 7, 0), &name);
  AT_CHECK(named(name, "EDT"));

  const int64_t local = localEpochS(kZoneEastern, utc(2026, 7, 26, 12), &name);
  AT_CHECK(named(name, "EDT"));
  AT_CHECK_EQ(local, utc(2026, 7, 26, 12) - 4 * 3600);
}

// Zones that do not observe DST must never be renamed or shifted — Arizona is
// the standing counter-example to "everyone springs forward".
AT_TEST(tz_zones_without_dst_never_move) {
  const char* name = nullptr;
  for (int month = 1; month <= 12; ++month) {
    const int64_t t = utc(2026, month, 15, 12);
    AT_CHECK_EQ(localEpochS(kZoneArizona, t, &name), t - 7 * 3600);
    AT_CHECK(named(name, "MST"));
    AT_CHECK_EQ(localEpochS(kZoneUtc, t, &name), t);
    AT_CHECK(named(name, "UTC"));
  }
}

// The year is taken in LOCAL time, not UTC: on New Year's Eve in the Americas
// the two disagree, and looking up March's transition in the wrong year is the
// classic form of this bug.
AT_TEST(tz_year_boundary_uses_local_year) {
  const char* name = nullptr;
  // 2027-01-01 03:00 UTC is still 2026-12-31 21:00 in Central. Either way it
  // is winter and the answer is CST — what must not happen is a wrong lookup.
  const int64_t local = localEpochS(kZoneCentral, utc(2027, 1, 1, 3), &name);
  AT_CHECK(named(name, "CST"));
  AT_CHECK_EQ(local, utc(2027, 1, 1, 3) - 6 * 3600);
}
