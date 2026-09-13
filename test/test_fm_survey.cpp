#include <cstdint>

#include "airtime/fm_survey.h"
#include "test_framework.h"

using namespace airtime;

namespace {
constexpr int64_t kS = 1000000;

// A simulated dial: signal strength per frequency, and which stations send
// clock time and how wrong they are.
struct Dial {
  struct Sta { int32_t khz; int rssi; uint16_t pi; bool ct; int64_t err_us; };
  std::size_t n = 0;
  Sta sta[8];

  int rssiAt(int32_t khz) const {
    for (std::size_t i = 0; i < n; ++i)
      if (sta[i].khz == khz) return sta[i].rssi;
    return 3;   // band noise
  }
  const Sta* at(int32_t khz) const {
    for (std::size_t i = 0; i < n; ++i)
      if (sta[i].khz == khz) return &sta[i];
    return nullptr;
  }
};

// Run a survey to completion against a dial. Returns the chosen frequencies.
std::size_t runSurvey(FmSurvey& s, const Dial& d, int32_t* out, std::size_t max,
                      int64_t step = 50000) {
  int64_t now = 0;
  s.begin(now);
  int32_t tuned = s.wantTuned();
  bool ct_done = false;

  for (int guard = 0; guard < 2000000 && !s.done(); ++guard) {
    now += step;
    // Halfway through a dwell, the station gets a chance to send clock time.
    if (s.phase() == SurveyPhase::Dwelling && !ct_done) {
      const Dial::Sta* st = d.at(tuned);
      if (st != nullptr && st->ct) {
        s.noteClockTime(st->pi, st->err_us);
        ct_done = true;
      }
    }
    if (s.tick(now, d.rssiAt(tuned))) {
      tuned = s.wantTuned();
      ct_done = false;
    }
  }
  return s.results(out, max);
}
}  // namespace

// The core promise: dropped somewhere new, the radio finds stations that know
// what time it is — without being told where it is.
AT_TEST(survey_finds_stations_that_carry_clock_time) {
  Dial d;
  d.n = 4;
  d.sta[0] = {8990, 45, 0xA920, true, 60000};     // good, 60 ms late
  d.sta[1] = {9310, 40, 0x1111, false, 0};        // strong, but no CT at all
  d.sta[2] = {10470, 38, 0x6E47, true, 90000};    // good, 90 ms late
  d.sta[3] = {10750, 30, 0x33CB, true, 120000};   // good, 120 ms late

  FmSurvey s;
  int32_t out[8];
  const std::size_t n = runSurvey(s, d, out, 8);

  AT_CHECK_EQ(n, 3u);                 // the CT-less station is not a result
  for (std::size_t i = 0; i < n; ++i) AT_CHECK(out[i] != 9310);
}

// Ranking is by AGREEMENT, not by strength. The dial survey found stations
// minutes to hours wrong booming in at full scale; loudness says nothing about
// whether a station knows the time.
AT_TEST(survey_ranks_by_agreement_not_signal) {
  Dial d;
  d.n = 3;
  d.sta[0] = {8990, 60, 0xAAAA, true, 5000000};   // loudest on the dial, 5 s out
  d.sta[1] = {9310, 20, 0xBBBB, true, 0};
  d.sta[2] = {10470, 22, 0xCCCC, true, 40000};

  FmSurvey s;
  int32_t out[8];
  const std::size_t n = runSurvey(s, d, out, 8);

  AT_CHECK(n >= 2);
  AT_CHECK(out[0] != 8990);           // the liar does not lead...
  for (std::size_t i = 0; i < n; ++i) AT_CHECK(out[i] != 8990);   // ...or appear
}

// The survey has to work on a clock that is merely STABLE, not correct — that
// is its whole situation, since it runs on a cold radio in a new town and is
// the very thing that will fix the clock. A shared error must cancel.
AT_TEST(survey_works_with_a_completely_wrong_clock) {
  const int64_t kWayOff = 3600LL * 1000000;   // an hour of common-mode error

  Dial good, bad;
  good.n = bad.n = 3;
  for (int i = 0; i < 3; ++i) {
    const int32_t khz[] = {8990, 9310, 10470};
    const int64_t err[] = {0, 40000, 80000};
    good.sta[i] = {khz[i], 40, (uint16_t)(0x1000 + i), true, err[i]};
    bad.sta[i]  = {khz[i], 40, (uint16_t)(0x1000 + i), true, err[i] + kWayOff};
  }

  FmSurvey s1, s2;
  int32_t a[8], b[8];
  const std::size_t na = runSurvey(s1, good, a, 8);
  const std::size_t nb = runSurvey(s2, bad, b, 8);

  AT_CHECK_EQ(na, nb);
  for (std::size_t i = 0; i < na; ++i) AT_CHECK_EQ(a[i], b[i]);   // same answer
}

// A station that disagrees with the crowd by minutes is excluded outright
// rather than merely ranked last — it is broken, not biased.
AT_TEST(survey_excludes_a_wildly_wrong_station) {
  Dial d;
  d.n = 4;
  d.sta[0] = {8990, 40, 0x1001, true, 0};
  d.sta[1] = {9310, 40, 0x1002, true, 50000};
  d.sta[2] = {10470, 40, 0x1003, true, 70000};
  d.sta[3] = {10750, 40, 0xDEAD, true, 400LL * 1000000};   // 400 s out

  FmSurvey s;
  int32_t out[8];
  const std::size_t n = runSurvey(s, d, out, 8);
  AT_CHECK_EQ(n, 3u);
  for (std::size_t i = 0; i < n; ++i) AT_CHECK(out[i] != 10750);
}

// An empty dial must finish and say nothing, not dwell on noise for an hour.
AT_TEST(survey_gives_up_on_an_empty_dial) {
  Dial d;   // no stations at all
  FmSurvey s;
  int32_t out[8];
  const std::size_t n = runSurvey(s, d, out, 8);
  AT_CHECK_EQ(n, 0u);
  AT_CHECK(s.done());
}

// The scan pass must not keep more than it was asked to dwell on: dwelling is
// 80 s per station and an unbounded list would run for hours.
AT_TEST(survey_keeps_only_the_strongest_candidates) {
  FmSurveyConfig cfg;
  cfg.max_candidates = 3;
  Dial d;
  d.n = 6;
  for (int i = 0; i < 6; ++i)
    d.sta[i] = {(int32_t)(8990 + i * 100), 20 + i * 5, (uint16_t)(0x2000 + i), true, i * 10000};

  FmSurvey s(cfg);
  int32_t out[8];
  runSurvey(s, d, out, 8);
  AT_CHECK(s.candidateCount() <= 3u);
  // ...and they are the strongest three, which are the last three of the six.
  for (std::size_t i = 0; i < s.candidateCount(); ++i)
    AT_CHECK(s.candidate(i).rssi >= 30);
}

AT_TEST(survey_reports_progress_monotonically) {
  Dial d;
  d.n = 2;
  d.sta[0] = {8990, 40, 0x1001, true, 0};
  d.sta[1] = {9310, 40, 0x1002, true, 20000};

  FmSurvey s;
  AT_CHECK_EQ(s.progressPct(), 0);
  int64_t now = 0;
  s.begin(now);
  int32_t tuned = s.wantTuned();
  int last = 0;
  for (int i = 0; i < 200000 && !s.done(); ++i) {
    now += 50000;
    if (s.tick(now, d.rssiAt(tuned))) tuned = s.wantTuned();
    const int p = s.progressPct();
    AT_CHECK(p >= last);
    last = p;
  }
  AT_CHECK_EQ(s.progressPct(), 100);
}
