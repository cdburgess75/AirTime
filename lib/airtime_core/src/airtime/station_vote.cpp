#include "station_vote.h"

namespace airtime {

static inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

void StationVoter::clear() { count_ = 0; }

bool StationVoter::add(const CtReport& r) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (reports_[i].pi == r.pi) {  // latest report per station wins
      reports_[i] = r;
      return true;
    }
  }
  if (count_ >= kMaxReports) return false;
  reports_[count_++] = r;
  return true;
}

void StationVoter::prune(int64_t cutoff_mono_us) {
  std::size_t w = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (reports_[i].rx_monotonic_us >= cutoff_mono_us) reports_[w++] = reports_[i];
  }
  count_ = w;
}

VoteResult StationVoter::vote(int64_t tolerance_us) const {
  VoteResult vr{false, 0, 0, static_cast<int>(count_)};
  if (count_ == 0) return vr;

  int64_t off[kMaxReports];
  for (std::size_t i = 0; i < count_; ++i) {
    off[i] = reports_[i].asserted_utc_us - reports_[i].rx_monotonic_us;
  }

  // Pick the cluster center (an existing offset) that gathers the most members
  // within tolerance. O(n^2) over a handful of stations — trivially fine.
  std::size_t best_center = 0;
  int best_n = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    int n = 0;
    for (std::size_t j = 0; j < count_; ++j) {
      if (iabs64(off[j] - off[i]) <= tolerance_us) ++n;
    }
    if (n > best_n) {
      best_n = n;
      best_center = i;
    }
  }

  // Gather and sort the winning cluster's offsets, then take the median.
  int64_t mem[kMaxReports];
  int m = 0;
  for (std::size_t j = 0; j < count_; ++j) {
    if (iabs64(off[j] - off[best_center]) <= tolerance_us) mem[m++] = off[j];
  }
  for (int a = 1; a < m; ++a) {  // insertion sort
    const int64_t key = mem[a];
    int b = a - 1;
    while (b >= 0 && mem[b] > key) {
      mem[b + 1] = mem[b];
      --b;
    }
    mem[b + 1] = key;
  }
  const int64_t median =
      (m & 1) ? mem[m / 2] : (mem[m / 2 - 1] + mem[m / 2]) / 2;

  vr.offset_us = median;
  vr.agreeing_stations = best_n;
  vr.has_consensus = best_n >= 2;
  return vr;
}

}  // namespace airtime
