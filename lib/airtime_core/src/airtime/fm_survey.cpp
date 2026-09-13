#include "fm_survey.h"

namespace airtime {

namespace {
int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }
}  // namespace

FmSurvey::FmSurvey(const FmSurveyConfig& cfg) : cfg_(cfg) {}

void FmSurvey::begin(int64_t now) {
  phase_ = SurveyPhase::Scanning;
  scan_khz_ = cfg_.start_khz;
  step_start_ = now;
  dwell_idx_ = 0;
  count_ = 0;
}

void FmSurvey::abort() { phase_ = SurveyPhase::Idle; }

int32_t FmSurvey::wantTuned() const {
  if (phase_ == SurveyPhase::Scanning) return scan_khz_;
  if (phase_ == SurveyPhase::Dwelling && dwell_idx_ < count_)
    return cand_[dwell_idx_].khz;
  return 0;
}

// Keep the strongest max_candidates, in descending strength. A plain insertion
// sort: the list is at most sixteen long and this runs once per channel.
void FmSurvey::insertCandidate(int32_t khz, int rssi) {
  if (rssi < cfg_.min_rssi) return;

  std::size_t cap = static_cast<std::size_t>(cfg_.max_candidates);
  if (cap > kMaxCandidates) cap = kMaxCandidates;
  if (cap == 0) return;

  if (count_ == cap && rssi <= cand_[count_ - 1].rssi) return;  // too weak

  std::size_t pos = 0;
  while (pos < count_ && cand_[pos].rssi >= rssi) ++pos;
  const std::size_t last = (count_ < cap) ? count_ : cap - 1;
  for (std::size_t i = last; i > pos; --i) cand_[i] = cand_[i - 1];

  cand_[pos] = FmCandidate{};
  cand_[pos].khz = khz;
  cand_[pos].rssi = rssi;
  if (count_ < cap) ++count_;
}

bool FmSurvey::tick(int64_t now, int rssi) {
  switch (phase_) {
    case SurveyPhase::Idle:
    case SurveyPhase::Done:
      return false;

    case SurveyPhase::Scanning: {
      if ((now - step_start_) < cfg_.scan_dwell_us) return false;
      insertCandidate(scan_khz_, rssi);
      scan_khz_ += cfg_.step_khz;
      step_start_ = now;
      if (scan_khz_ > cfg_.end_khz) {
        // Nothing on the dial at all: say so rather than dwelling on noise.
        if (count_ == 0) { finish(); return false; }
        phase_ = SurveyPhase::Dwelling;
        dwell_idx_ = 0;
      }
      return true;   // retune, either to the next channel or the first dwell
    }

    case SurveyPhase::Dwelling: {
      if ((now - step_start_) < cfg_.ct_dwell_us) return false;
      ++dwell_idx_;
      step_start_ = now;
      if (dwell_idx_ >= count_) { finish(); return true; }
      return true;
    }
  }
  return false;
}

void FmSurvey::noteClockTime(uint16_t pi, int64_t error_us) {
  if (phase_ != SurveyPhase::Dwelling || dwell_idx_ >= count_) return;
  FmCandidate& c = cand_[dwell_idx_];
  c.pi = pi;
  c.offset_us = error_us;
  c.heard_ct = true;
}

void FmSurvey::finish() {
  phase_ = SurveyPhase::Done;

  // Re-reference every offset to the MEDIAN of the stations that reported one.
  //
  // This is what lets the survey run on a clock that is merely stable rather
  // than correct — which is the situation on a cold radio in a new town, where
  // the survey is the very thing that will fix the clock. A shared error
  // cancels; only how far each station sits from the crowd survives.
  int64_t offs[kMaxCandidates];
  std::size_t n = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (cand_[i].heard_ct) offs[n++] = cand_[i].offset_us;
  }
  if (n == 0) return;

  for (std::size_t a = 1; a < n; ++a) {   // insertion sort
    const int64_t key = offs[a];
    std::size_t b = a;
    while (b > 0 && offs[b - 1] > key) { offs[b] = offs[b - 1]; --b; }
    offs[b] = key;
  }
  const int64_t median = (n & 1) ? offs[n / 2] : (offs[n / 2 - 1] + offs[n / 2]) / 2;

  for (std::size_t i = 0; i < count_; ++i) {
    if (cand_[i].heard_ct) cand_[i].offset_us -= median;
  }

  // Rank by agreement with the crowd, not by signal strength: the dial survey
  // found stations minutes to hours wrong booming in at full scale.
  for (std::size_t a = 1; a < count_; ++a) {
    const FmCandidate key = cand_[a];
    const int64_t key_rank = key.heard_ct ? iabs64(key.offset_us) : INT64_MAX;
    std::size_t b = a;
    while (b > 0) {
      const int64_t prev =
          cand_[b - 1].heard_ct ? iabs64(cand_[b - 1].offset_us) : INT64_MAX;
      if (prev <= key_rank) break;
      cand_[b] = cand_[b - 1];
      --b;
    }
    cand_[b] = key;
  }
}

std::size_t FmSurvey::results(int32_t* khz_out, std::size_t max) const {
  if (khz_out == nullptr || phase_ != SurveyPhase::Done) return 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < count_ && n < max; ++i) {
    if (!cand_[i].heard_ct) continue;
    if (iabs64(cand_[i].offset_us) > cfg_.max_error_us) continue;
    khz_out[n++] = cand_[i].khz;
  }
  return n;
}

int FmSurvey::progressPct() const {
  switch (phase_) {
    case SurveyPhase::Idle: return 0;
    case SurveyPhase::Done: return 100;
    case SurveyPhase::Scanning: {
      const int32_t span = cfg_.end_khz - cfg_.start_khz;
      if (span <= 0) return 0;
      // The scan is the fast half but a small share of the wall clock; weight
      // it accordingly so the bar does not sit at 90% for ten minutes.
      const int32_t done = scan_khz_ - cfg_.start_khz;
      return static_cast<int>((int64_t)done * 20 / span);
    }
    case SurveyPhase::Dwelling: {
      if (count_ == 0) return 20;
      return 20 + static_cast<int>((int64_t)dwell_idx_ * 80 / (int64_t)count_);
    }
  }
  return 0;
}

}  // namespace airtime
