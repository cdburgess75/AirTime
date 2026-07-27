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

// ── Anchored schedules ──────────────────────────────────────────────────────
//
// The published-UTC trap, which is the reason NetAnchor exists. A net that
// meets at 6:30 PM Central meets at 6:30 PM Central all year; it is the UTC
// time that moves, 2330Z in summer and 0030Z in winter. A directory that quotes
// only UTC has frozen one half of the year — and the SkyWave list this
// firmware's table came from says so explicitly about itself.
//
// 2026-07-15 is inside US DST; 2026-01-15 is not.
AT_TEST(nets_anchored_to_local_time_survive_the_dst_boundary) {
  const HamNet la[] = {{"LA Traffic", 3910, NetMode::Lsb, kDaily,
                        18 * 60 + 30, 60, NetAnchor::UsCentral}};

  // Summer: 18:30 CDT is 23:30 UTC.
  AT_CHECK_EQ(netActiveAt(la, 1, utc(2026, 7, 15, 23, 45)), 0);
  AT_CHECK_EQ(netActiveAt(la, 1, utc(2026, 7, 16,  0, 45)), -1);

  // Winter: the SAME net is 00:30 UTC the following day. A table of frozen UTC
  // times would have it on the air at 23:45 in January, an hour early, and
  // silent when it actually starts.
  AT_CHECK_EQ(netActiveAt(la, 1, utc(2026, 1, 15, 23, 45)), -1);
  AT_CHECK_EQ(netActiveAt(la, 1, utc(2026, 1, 16,  0, 45)), 0);
}

// Eastern and Central are an hour apart in both seasons, so a net anchored to
// the wrong one is wrong all year rather than half of it.
AT_TEST(nets_distinguish_eastern_from_central) {
  const HamNet e[] = {{"GA SSB", 3975, NetMode::Lsb, kDaily,
                       19 * 60, 60, NetAnchor::UsEastern}};
  const HamNet c[] = {{"MS Phone", 3862, NetMode::Lsb, kDaily,
                       19 * 60, 60, NetAnchor::UsCentral}};
  // 19:00 EDT = 23:00Z; 19:00 CDT = 00:00Z next day.
  AT_CHECK_EQ(netActiveAt(e, 1, utc(2026, 7, 15, 23, 30)), 0);
  AT_CHECK_EQ(netActiveAt(c, 1, utc(2026, 7, 15, 23, 30)), -1);
  AT_CHECK_EQ(netActiveAt(c, 1, utc(2026, 7, 16,  0, 30)), 0);
}

// A UTC-anchored net must be left alone — anchoring is opt-in, and the default
// keeps every table written before NetAnchor existed meaning what it said.
AT_TEST(nets_default_to_utc_unchanged_by_season) {
  const HamNet mmsn[] = {{"MMSN", 14300, NetMode::Usb, kDaily, 16 * 60, 600}};
  AT_CHECK_EQ(netActiveAt(mmsn, 1, utc(2026, 7, 15, 16, 30)), 0);
  AT_CHECK_EQ(netActiveAt(mmsn, 1, utc(2026, 1, 15, 16, 30)), 0);
  AT_CHECK_EQ(netActiveAt(mmsn, 1, utc(2026, 1, 15, 15, 30)), -1);
}

// The day mask travels with the anchor. A Saturday-evening Central net running
// past midnight is on the air at 01:00 UTC Sunday, and must not be found by
// looking up "Saturday" in UTC.
AT_TEST(nets_apply_the_day_mask_in_the_anchor_zone) {
  // 2026-07-18 is a Saturday. 19:30 CDT Sat = 00:30 UTC Sunday.
  const HamNet sat[] = {{"Sat Central", 3985, NetMode::Lsb, kSat,
                         19 * 60 + 30, 60, NetAnchor::UsCentral}};
  AT_CHECK_EQ(netActiveAt(sat, 1, utc(2026, 7, 19, 0, 45)), 0);   // Sun UTC
  AT_CHECK_EQ(netActiveAt(sat, 1, utc(2026, 7, 18, 0, 45)), -1);  // Sat UTC
}

// A net with no schedule is never on the air and never next. The Hurricane
// Watch Net is listed 0000-2400 by its source so it stays visible in a UI; on a
// clock that would be a 24-hour-a-day claim that a storm net is up.
AT_TEST(nets_on_demand_entries_never_claim_to_be_on_air) {
  const HamNet hw[] = {{"Hurricane", 14325, NetMode::Usb, kOnDemand, 0, 1440}};
  AT_CHECK_EQ(netActiveAt(hw, 1, utc(2026, 7, 15, 12, 0)), -1);
  AT_CHECK_EQ(netActiveAt(hw, 1, utc(2026, 1, 1, 3, 0)), -1);
  int wait = -1;
  AT_CHECK_EQ(netNextAt(hw, 1, utc(2026, 7, 15, 12, 0), &wait), -1);
}
