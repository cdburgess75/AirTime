#include <cstdint>

#include "airtime/nets.h"
#include "airtime/rds_ct.h"
#include "test_framework.h"

using namespace airtime;

namespace {
int64_t utc(int y, int mo, int d, int h = 0, int mi = 0) {
  return (static_cast<int64_t>(civilToMjd(y, mo, d)) - 40587) * 86400 +
         h * 3600 + mi * 60;
}

// 2026-07-26 is a Sunday; 2026-07-29 a Wednesday.
const HamNet kNets[] = {
    {"Daily Noon",  14300, NetMode::Usb, kDaily,    12 * 60,      120},
    {"Wed Only",     7200, NetMode::Lsb, kWed,      20 * 60,       60},
    {"Late Night",   3985, NetMode::Lsb, kDaily,    23 * 60 + 30,  60},
    {"Weekday CW",   7047, NetMode::Cw,  kWeekdays, 14 * 60,       30},
};
constexpr std::size_t kCount = sizeof(kNets) / sizeof(kNets[0]);
}  // namespace

AT_TEST(nets_weekday_matches_the_calendar) {
  AT_CHECK_EQ(utcWeekday(utc(2026, 7, 26)), 0);   // Sunday
  AT_CHECK_EQ(utcWeekday(utc(2026, 7, 29)), 3);   // Wednesday
  AT_CHECK_EQ(utcWeekday(utc(2026, 1, 1)), 4);    // Thursday
}

AT_TEST(nets_finds_what_is_on_the_air) {
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 12, 30)), 0);
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 11, 59)), -1);
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 13, 59)), 0);
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 14, 1)), -1);  // Sun
}

// A net listed for one weekday must not appear on the others.
AT_TEST(nets_respect_their_day_mask) {
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 29, 20, 15)), 1);  // Wed
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 30, 20, 15)), -1); // Thu
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 29, 14, 10)), 3);  // Wed CW
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 14, 10)), -1); // Sun
}

// A net that starts at 2330 and runs an hour is still on the air at 0015 the
// next morning. Forgetting that leaves the screen claiming nothing is on while
// the net is mid-sentence.
AT_TEST(nets_handle_sessions_that_cross_midnight) {
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 26, 23, 45)), 2);
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 27, 0, 15)), 2);
  AT_CHECK_EQ(netActiveAt(kNets, kCount, utc(2026, 7, 27, 0, 31)), -1);
}

// ...including across the week boundary, which is the same bug one day later:
// Saturday 2330 runs into Sunday, and Sunday is where the week restarts.
AT_TEST(nets_handle_sessions_that_cross_the_week_boundary) {
  const HamNet sat[] = {{"Sat Late", 3985, NetMode::Lsb, kSat, 23 * 60 + 30, 60}};
  AT_CHECK_EQ(netActiveAt(sat, 1, utc(2026, 8, 1, 23, 45)), 0);   // Saturday
  AT_CHECK_EQ(netActiveAt(sat, 1, utc(2026, 8, 2, 0, 15)), 0);    // into Sunday
  AT_CHECK_EQ(netActiveAt(sat, 1, utc(2026, 8, 2, 1, 0)), -1);
}

AT_TEST(nets_report_the_wait_until_the_next_one) {
  int wait = -1;
  // Sunday 10:00 — the daily noon net is two hours out.
  AT_CHECK_EQ(netNextAt(kNets, kCount, utc(2026, 7, 26, 10, 0), &wait), 0);
  AT_CHECK_EQ(wait, 120);

  // While something is on the air, "next" is that thing, with zero wait.
  AT_CHECK_EQ(netNextAt(kNets, kCount, utc(2026, 7, 26, 12, 30), &wait), 0);
  AT_CHECK_EQ(wait, 0);
}

// A weekly net must still be found from the wrong day — the search looks a full
// week ahead rather than only at today.
AT_TEST(nets_find_a_weekly_net_from_days_away) {
  const HamNet wed[] = {{"Wed Only", 7200, NetMode::Lsb, kWed, 20 * 60, 60}};
  int wait = -1;
  // Thursday 21:00 -> next Wednesday 20:00 is 5 days 23 hours away.
  AT_CHECK_EQ(netNextAt(wed, 1, utc(2026, 7, 30, 21, 0), &wait), 0);
  AT_CHECK_EQ(wait, 5 * 1440 + 23 * 60);
}

AT_TEST(nets_handle_an_empty_or_broken_table) {
  int wait = -1;
  AT_CHECK_EQ(netActiveAt(nullptr, 0, utc(2026, 7, 26, 12, 0)), -1);
  AT_CHECK_EQ(netNextAt(kNets, 0, utc(2026, 7, 26, 12, 0), &wait), -1);
  const HamNet junk[] = {{"No Days", 7200, NetMode::Lsb, 0, 0, 60},
                         {"No Time", 7200, NetMode::Lsb, kDaily, 0, 0}};
  AT_CHECK_EQ(netActiveAt(junk, 2, utc(2026, 7, 26, 0, 0)), -1);
}
