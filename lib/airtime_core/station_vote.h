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
// A "report" says: at local monotonic time rx_monotonic_ms, station `pi`
// asserted UTC was asserted_utc_ms. The implied clock offset is therefore
// (asserted_utc_ms - rx_monotonic_ms). Stations that agree produce offsets that
// cluster; a lone wrong station sits alone and is outvoted.
//
// Fixed-capacity, zero-heap — suitable for the firmware as-is.

#include <cstddef>
#include <cstdint>

namespace airtime {

struct CtReport {
  uint16_t pi;             // station PI code (station identity; dedup key)
  int64_t asserted_utc_ms; // UTC the station reported, in ms
  int64_t rx_monotonic_ms; // local monotonic time at reception, in ms
};

struct VoteResult {
  bool has_consensus;    // true iff >=2 distinct stations agree within tolerance
  int64_t offset_ms;     // consensus clock offset (median of the winning cluster)
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
  // tolerance_ms of a common member, maximizing distinct-station count.
  // With one report: has_consensus=false but offset_ms/agreeing_stations still
  // describe that single source (usable for small single-source slews).
  VoteResult vote(int64_t tolerance_ms) const;

  std::size_t size() const { return count_; }

 private:
  CtReport reports_[kMaxReports];
  std::size_t count_ = 0;
};

}  // namespace airtime
