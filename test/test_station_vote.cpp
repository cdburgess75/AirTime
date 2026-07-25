#include "airtime/station_vote.h"
#include "test_framework.h"

using airtime::CtReport;
using airtime::StationVoter;
using airtime::VoteResult;

namespace {
// rx_monotonic fixed at 0 so the implied offset equals asserted_utc_us.
CtReport rep(uint16_t pi, int64_t utc_ms) { return CtReport{pi, utc_ms, 0}; }
}  // namespace

// Two stations agreeing within tolerance => consensus, median offset.
AT_TEST(vote_two_agree) {
  StationVoter v;
  v.add(rep(0x1001, 100));
  v.add(rep(0x1002, 150));
  const VoteResult r = v.vote(/*tolerance_us=*/200);
  AT_CHECK(r.has_consensus);
  AT_CHECK_EQ(r.agreeing_stations, 2);
  AT_CHECK_EQ(r.offset_us, 125);  // median of {100,150}
}

// A lone outlier is outvoted by the agreeing majority and excluded from median.
AT_TEST(vote_rejects_outlier) {
  StationVoter v;
  v.add(rep(0x1001, 100));
  v.add(rep(0x1002, 120));
  v.add(rep(0x1003, 5000));  // wrong CT from one station
  const VoteResult r = v.vote(200);
  AT_CHECK(r.has_consensus);
  AT_CHECK_EQ(r.agreeing_stations, 2);
  AT_CHECK_EQ(r.total_reports, 3);
  AT_CHECK_EQ(r.offset_us, 110);
}

// A single station never reaches consensus, but its offset is still reported
// (the arbiter may still use it for a small single-source slew).
AT_TEST(vote_single_station_no_consensus) {
  StationVoter v;
  v.add(rep(0x2001, 250));
  const VoteResult r = v.vote(200);
  AT_CHECK(!r.has_consensus);
  AT_CHECK_EQ(r.agreeing_stations, 1);
  AT_CHECK_EQ(r.offset_us, 250);
}

// No two stations agree => no consensus.
AT_TEST(vote_all_disagree) {
  StationVoter v;
  v.add(rep(0x3001, 0));
  v.add(rep(0x3002, 1000));
  v.add(rep(0x3003, 2000));
  const VoteResult r = v.vote(100);
  AT_CHECK(!r.has_consensus);
  AT_CHECK_EQ(r.agreeing_stations, 1);
}

// The latest report from a PI replaces its earlier one (no double-counting).
AT_TEST(vote_dedup_by_station) {
  StationVoter v;
  v.add(rep(0x4001, 100));
  v.add(rep(0x4001, 900));  // same station, updated CT
  AT_CHECK_EQ(v.size(), 1u);
  v.add(rep(0x4002, 880));
  const VoteResult r = v.vote(100);
  AT_CHECK(r.has_consensus);          // 900 and 880 agree
  AT_CHECK_EQ(r.agreeing_stations, 2);
  AT_CHECK_EQ(r.offset_us, 890);
}

// Empty voter is well-defined.
AT_TEST(vote_empty) {
  StationVoter v;
  const VoteResult r = v.vote(100);
  AT_CHECK(!r.has_consensus);
  AT_CHECK_EQ(r.agreeing_stations, 0);
  AT_CHECK_EQ(r.total_reports, 0);
}
