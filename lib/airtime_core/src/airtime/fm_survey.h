#pragma once
//
// Finding usable clock-time stations without being told where you are.
//
// The FM station list has been a compile-time constant baked from one evening's
// measurements on one dial in New Orleans. Carry the radio two states over and
// RDS never seeds, and the whole clock collapses to nothing — which makes the
// device a demo rather than an instrument. The owner named this early:
// location, antenna, propagation and time of day are all variables.
//
// So the radio surveys for itself, in two passes, for the same reason the
// bench tool did:
//
//   SCAN  — step the dial fast, keeping only signal strength. A full band at
//           75 s per channel is two hours; at ~200 ms it is twenty seconds.
//           Most of the dial is empty and deserves no more than that.
//   DWELL — sit on the strongest candidates long enough for a clock-time group
//           to arrive. RDS sends group 4A about ONCE A MINUTE, so this is
//           necessarily slow and there is no way to hurry it.
//
// Ranking is by agreement, not by strength. The survey found stations that were
// minutes to hours wrong transmitting perfectly good signals; loudness says
// nothing about whether a station knows what time it is. Candidates are scored
// against the median of everything heard, so the crowd defines truth and the
// outliers fall out — the same principle as StationVoter, applied to the dial
// instead of to a moment.
//
// Pure logic: it asks to be tuned somewhere and is told what happened. It does
// not touch a radio, which is what makes it testable.

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

struct FmSurveyConfig {
  int32_t start_khz = 8810;   // 88.1 MHz in SI4735 10 kHz units: 8810
  int32_t end_khz = 10790;    // 107.9
  int32_t step_khz = 20;      // 200 kHz — the US/CA channel raster
  int64_t scan_dwell_us = 200000;   // long enough for the tuner to settle
  // A clock-time group comes about once a minute, so a dwell shorter than that
  // only sometimes catches one. 80 s clears the minute with margin.
  int64_t ct_dwell_us = 80LL * 1000000;
  int max_candidates = 10;    // strongest N kept from the scan pass
  int min_rssi = 12;          // below this there is nothing to decode
  // Beyond this a station is not biased, it is broken (the dial survey found
  // sets minutes to hours out). Excluded rather than ranked.
  int64_t max_error_us = 3000000;
};

struct FmCandidate {
  int32_t khz = 0;
  int rssi = 0;
  uint16_t pi = 0;
  bool heard_ct = false;
  int64_t offset_us = 0;   // its clock-time error against the survey's own median
};

enum class SurveyPhase { Idle, Scanning, Dwelling, Done };

class FmSurvey {
 public:
  static constexpr std::size_t kMaxCandidates = 16;

  explicit FmSurvey(const FmSurveyConfig& cfg = FmSurveyConfig{});

  void begin(int64_t now);
  void abort();

  // Frequency the caller should have tuned right now. 0 when idle or finished.
  int32_t wantTuned() const;

  // Drive the survey. Call every loop with the current signal strength; returns
  // true when the tuned frequency has changed and the caller must retune.
  bool tick(int64_t now, int rssi);

  // A clock-time group arrived on the currently dwelled station. `error_us` is
  // how far its assertion sat from the caller's own clock — a raw number; the
  // survey re-references everything to its own median at the end, so the
  // caller's clock only has to be STABLE, not correct.
  void noteClockTime(uint16_t pi, int64_t error_us);

  SurveyPhase phase() const { return phase_; }
  bool done() const { return phase_ == SurveyPhase::Done; }
  // 0..100, for a progress line on the display.
  int progressPct() const;

  // Best stations, most trustworthy first. Only ones that actually delivered
  // clock time and agreed with the crowd. Returns how many were written.
  std::size_t results(int32_t* khz_out, std::size_t max) const;

  std::size_t candidateCount() const { return count_; }
  const FmCandidate& candidate(std::size_t i) const { return cand_[i]; }

 private:
  void finish();
  void insertCandidate(int32_t khz, int rssi);

  FmSurveyConfig cfg_;
  SurveyPhase phase_ = SurveyPhase::Idle;
  int64_t step_start_ = 0;
  int32_t scan_khz_ = 0;
  std::size_t dwell_idx_ = 0;

  FmCandidate cand_[kMaxCandidates];
  std::size_t count_ = 0;
};

}  // namespace airtime
