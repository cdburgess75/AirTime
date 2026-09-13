#pragma once
//
// Per-station clock-time bias — WWV teaching RDS.
//
// ── The observation this is built on ────────────────────────────────────────
//
// An FM station's clock-time is not noisy, it is WRONG BY A CONSTANT. Measured
// at the owner's QTH across separate sessions days apart: 89.9 late by 37-87 ms,
// 104.7 by 216 ms, 107.5 by 352-367 ms, and on one evening the whole crowd
// drifted together to 728 ms. Each station repeats its own error faithfully —
// encoder latency, studio automation, playout buffering — and averaging more
// reports from the same station cannot remove it.
//
// WWV knows exactly where the minute begins but visits once an hour. RDS is
// continuous but biased. So: while the clock is WWV-good, measure how late each
// station runs; afterwards, subtract it. A station that is reliably 728 ms late
// becomes a source that is right.
//
// ── Why this matters more than the milliseconds suggest ─────────────────────
//
// Without it a badly-biased station does not merely degrade the clock — it gets
// VETOED. Once WWV pulls the clock onto the true minute, every correction that
// station asks for exceeds the arbiter's 500 ms step threshold, so §4 rule 3
// rejects each one, and the device is left running on WWV alone: one fix an
// hour, nothing in between, and a whole night's coasting if propagation closes.
// Correcting the bias hands that continuous source back.
//
// ── What this deliberately is not ───────────────────────────────────────────
//
// Not an outlier filter — StationVoter already does that, and a station that is
// minutes or hours out is broken rather than biased (max_bias_us draws the
// line). Not a substitute for voting: biases are learned independently per
// station and the voter still has to agree.

#include <cstddef>
#include <cstdint>

namespace airtime {

struct StationBiasConfig {
  // A correction is withheld until the station has been measured this many
  // times, so one lucky sample cannot start steering the clock.
  int min_samples = 3;
  // EMA weight per observation. Slow on purpose: the quantity is a constant,
  // so there is nothing to track quickly, and a slow filter is what keeps one
  // bad measurement from undoing hours of good ones.
  double alpha = 0.25;
  // Beyond this a station is not biased, it is broken — the survey found sets
  // that were minutes to hours out. Refuse to "correct" those into agreement;
  // let the voter throw them away instead.
  int64_t max_bias_us = 2000000;
};

struct StationBias {
  uint16_t pi = 0;          // RDS programme identification — the station's identity
  int64_t bias_us = 0;      // positive: this station's clock-time arrives LATE
  int samples = 0;
  int64_t last_obs_mono = 0;
};

class StationBiasTable {
 public:
  static constexpr std::size_t kMaxStations = 8;

  explicit StationBiasTable(const StationBiasConfig& cfg = StationBiasConfig{});

  // Record how late this station's assertion was, measured against a clock the
  // caller believes. The caller owns that judgement — see
  // AppConfig::station_bias_learn_below_us.
  void observe(uint16_t pi, int64_t lateness_us, int64_t mono_us);

  // Amount to ADD to this station's asserted UTC to put it on time.
  // Zero until the station has been measured min_samples times.
  int64_t correction(uint16_t pi) const;

  // Seed from persisted storage without waiting to re-measure (a station's
  // bias is a property of the transmitter, so it is worth keeping across a
  // power cycle).
  void seed(uint16_t pi, int64_t bias_us, int samples);

  std::size_t count() const { return count_; }
  const StationBias& at(std::size_t i) const { return rows_[i]; }
  const StationBias* find(uint16_t pi) const;

 private:
  StationBias* row(uint16_t pi);

  StationBiasConfig cfg_;
  StationBias rows_[kMaxStations];
  std::size_t count_ = 0;
};

}  // namespace airtime
