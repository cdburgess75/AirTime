#pragma once
//
// Multi-station RDS clock-time voting.
//
// PLAN.md §4 Tier 1: "Multi-station VOTING mandatory (many US stations send no
// CT group; some send wrong/offset CT)." Also arbiter rule 3: corrections of
// >=500 ms require two independent sources agreeing. This module collects the
// latest CT report from each receivable station and reports the consensus clock
// offset plus how many distinct stations agree.
//
// A "report" says: station `pi` asserted UTC was asserted_utc_us at a moment
// when our own reference read reference_us. The implied clock offset is their
// difference. Stations that agree produce offsets that cluster; a lone wrong
// station sits alone and is outvoted.
//
// Fixed-capacity, zero-heap — suitable for the firmware as-is.

#include <cstddef>
#include <cstdint>

namespace airtime {

struct CtReport {
  uint16_t pi;             // station PI code (station identity; dedup key)
  int64_t asserted_utc_us; // UTC the station reported (µs)

  // What our clock believed at the instant this group arrived. The implied
  // offset is `asserted_utc_us - reference_us`, i.e. the clock's error measured
  // *at reception*.
  //
  // This must NOT be a value projected forward to "now". Doing so makes the
  // implied offset carry (report age x clock rate error), and since that bias is
  // itself proportional to the rate error, it cancels exactly the quantity the
  // drift estimator is trying to observe. Measured consequence: the estimator
  // converged to 21.2 ppm against a true 28 ppm and froze there, with a constant
  // -813 us error, because ~120 s mean report age x 6.8 ppm residual = 812 us.
  //
  // Before the clock is set there is nothing to compare against, so callers pass
  // the monotonic reception time instead; the offset is then the raw
  // monotonic->UTC mapping, which is what seeds the clock.
  int64_t reference_us;

  int64_t rx_monotonic_us; // monotonic reception time — used only for ageing
};

struct VoteResult {
  bool has_consensus;    // true iff >=2 distinct stations agree within tolerance
  int64_t offset_us;     // consensus clock offset (median of the winning cluster)
  int agreeing_stations; // distinct stations in the winning cluster
  int total_reports;     // reports considered
};

class StationVoter {
 public:
  static constexpr std::size_t kMaxReports = 32;

  void clear();

  // Record the latest report for a station, replacing any prior report from the
  // same PI. Returns false only if capacity is exceeded by a *new* station.
  bool add(const CtReport& r);

  // Winning cluster = the set of reports whose implied offsets all fall within
  // tolerance_us of a common member, maximizing distinct-station count.
  // With one report: has_consensus=false but offset_us/agreeing_stations still
  // describe that single source (usable for small single-source slews).
  VoteResult vote(int64_t tolerance_us) const;

  // Drop reports received before `cutoff_mono_us`, so a station that goes off
  // air (or drifts out of range) stops voting instead of carrying a stale
  // assertion forever.
  void prune(int64_t cutoff_mono_us);

  std::size_t size() const { return count_; }

 private:
  CtReport reports_[kMaxReports];
  std::size_t count_ = 0;
};

}  // namespace airtime
