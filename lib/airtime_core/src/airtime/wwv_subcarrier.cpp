#include "airtime/wwv_subcarrier.h"

namespace airtime {

namespace {
// Windows within one second, measured from the pulse's opening edge. Every
// symbol is ON through the first and OFF through the last; the middle two tell
// the symbols apart (zero 170 ms, one 470 ms, marker 770 ms).
constexpr int kOnMs0 = 0, kOnMs1 = 150;
constexpr int kOneMs0 = 250, kOneMs1 = 450;
constexpr int kMarkMs0 = 550, kMarkMs1 = 750;
constexpr int kOffMs0 = 800, kOffMs1 = 1000;
}  // namespace

SubcarrierSecondReader::SubcarrierSecondReader(const SubcarrierReaderConfig& cfg)
    : cfg_(cfg) {
  int64_t b = cfg_.block_us > 0 ? 1000000 / cfg_.block_us : 20;
  if (b < 10) b = 10;   // the windows need at least 100 ms bins
  if (b > kMaxBins) b = kMaxBins;
  bins_ = static_cast<int>(b);
  bin_us_ = 1000000 / bins_;
}

void SubcarrierSecondReader::reset() {
  have_origin_ = false;
  origin_us_ = 0;
  cur_bin_ = -1;
  cur_sum_ = 0.0f;
  cur_n_ = 0;
  cur_stamp_ = 0;
  for (int i = 0; i < 2 * kMaxBins; ++i) {
    val_[i] = 0.0f;
    have_[i] = false;
    stamp_[i] = 0;
  }
  for (int i = 0; i < kMaxBins; ++i) {
    fold_[i] = 0.0f;
    fold_have_[i] = false;
  }
  phase_ = -1;
  cand_phase_ = -1;
  cand_run_ = 0;
  on_ = 0.0f;
  off_ = 0.0f;
  q_head_ = 0;
  q_len_ = 0;
  diag_.locked = false;
  diag_.phase_bin = -1;
  diag_.contrast = 0.0f;
  diag_.on_level = 0.0f;
  diag_.off_level = 0.0f;
}

void SubcarrierSecondReader::process(int64_t mono_us, real power) {
  ++diag_.blocks;
  if (power > diag_.max_power) diag_.max_power = power;

  if (!have_origin_) {
    have_origin_ = true;
    origin_us_ = mono_us;
  }
  const int64_t rel = mono_us - origin_us_;
  if (rel < 0) return;  // older than where this run started
  const int64_t b = rel / bin_us_;

  if (cur_bin_ < 0) {
    cur_bin_ = b;
  } else if (b < cur_bin_) {
    return;  // out of order
  } else if (b > cur_bin_) {
    finalizeBin(cur_bin_, cur_n_ > 0,
                cur_n_ > 0 ? cur_sum_ / static_cast<real>(cur_n_) : 0.0f, cur_stamp_);
    if (b - cur_bin_ > 4 * bins_) {
      // Four silent seconds: the sampler stopped, or the band did. Whatever
      // the fold held describes a signal that is gone.
      reset();
      have_origin_ = true;
      origin_us_ = mono_us;
      cur_bin_ = 0;
    } else {
      // Bins that got no block (the sampler drops some) are holes, and are
      // finalized as such so every second still reaches the decoder.
      for (int64_t k = cur_bin_ + 1; k < b; ++k) {
        finalizeBin(k, false, 0.0f, origin_us_ + k * bin_us_);
      }
      cur_bin_ = b;
    }
    cur_n_ = 0;
    cur_sum_ = 0.0f;
  }

  if (cur_n_ == 0) cur_stamp_ = mono_us;
  cur_sum_ += power;
  ++cur_n_;
}

bool SubcarrierSecondReader::next(int64_t* pulse_ms, int64_t* edge_us) {
  if (q_len_ == 0) return false;
  *pulse_ms = queue_[q_head_].pulse_ms;
  *edge_us = queue_[q_head_].edge_us;
  q_head_ = (q_head_ + 1) % kQueue;
  --q_len_;
  return true;
}

void SubcarrierSecondReader::push(int64_t pulse_ms, int64_t edge_us) {
  if (q_len_ == kQueue) {  // nobody drained a minute of seconds: drop the oldest
    q_head_ = (q_head_ + 1) % kQueue;
    --q_len_;
  }
  queue_[(q_head_ + q_len_) % kQueue] = Out{pulse_ms, edge_us};
  ++q_len_;
}

void SubcarrierSecondReader::finalizeBin(int64_t abs_bin, bool have, real value,
                                         int64_t stamp) {
  const int ring = 2 * bins_;
  const int idx = static_cast<int>(abs_bin % ring);
  val_[idx] = value;
  have_[idx] = have;
  stamp_[idx] = stamp;

  const int k = static_cast<int>(abs_bin % bins_);
  if (have) {
    if (!fold_have_[k]) {
      fold_[k] = value;
      fold_have_[k] = true;
    } else {
      fold_[k] += cfg_.fold_alpha * (value - fold_[k]);
    }
  }

  if (k == bins_ - 1) evaluatePhase();

  // A second that opened at the locked position has just had its last bin.
  if (phase_ >= 0 && k == (phase_ + bins_ - 1) % bins_ && abs_bin >= bins_ - 1) {
    readSecond(abs_bin - (bins_ - 1));
  }
}

real SubcarrierSecondReader::foldWindow(int phase, int ms0, int ms1) const {
  const int bin_ms = static_cast<int>(bin_us_ / 1000);
  const int k0 = ms0 / bin_ms;
  const int k1 = ms1 / bin_ms;
  real sum = 0.0f;
  int n = 0;
  for (int k = k0; k < k1; ++k) {
    sum += fold_[(phase + k) % bins_];
    ++n;
  }
  return n > 0 ? sum / static_cast<real>(n) : 0.0f;
}

real SubcarrierSecondReader::foldContrast(int phase) const {
  const real off = foldWindow(phase, kOffMs0, kOffMs1);
  return off > 0.0f ? foldWindow(phase, kOnMs0, kOnMs1) / off : 0.0f;
}

void SubcarrierSecondReader::evaluatePhase() {
  for (int k = 0; k < bins_; ++k) {
    if (!fold_have_[k]) return;
  }

  int best = -1;
  real best_c = 0.0f;
  for (int ph = 0; ph < bins_; ++ph) {
    const real c = foldContrast(ph);
    if (c > best_c) {
      best_c = c;
      best = ph;
    }
  }
  diag_.contrast = best_c;

  // Levels follow the fold while locked, so a slow fade or a band opening up
  // moves the midpoint with it.
  if (phase_ >= 0) {
    on_ = foldWindow(phase_, kOnMs0, kOnMs1);
    off_ = foldWindow(phase_, kOffMs0, kOffMs1);
    diag_.on_level = on_;
    diag_.off_level = off_;
  }

  const bool wants_move =
      best >= 0 && best != phase_ && best_c >= cfg_.lock_contrast &&
      (phase_ < 0 || best_c >= foldContrast(phase_) * cfg_.relock_margin);
  if (!wants_move) {
    cand_phase_ = -1;
    cand_run_ = 0;
    return;
  }
  if (best == cand_phase_) {
    ++cand_run_;
  } else {
    cand_phase_ = best;
    cand_run_ = 1;
  }
  if (cand_run_ < cfg_.lock_hold_s) return;

  phase_ = best;
  cand_phase_ = -1;
  cand_run_ = 0;
  on_ = foldWindow(best, kOnMs0, kOnMs1);
  off_ = foldWindow(best, kOffMs0, kOffMs1);
  // Seconds read against the old position must not run into the new one.
  q_head_ = 0;
  q_len_ = 0;
  ++diag_.locks;
  diag_.locked = true;
  diag_.phase_bin = best;
  diag_.on_level = on_;
  diag_.off_level = off_;
}

bool SubcarrierSecondReader::windowMean(int64_t start_bin, int ms0, int ms1,
                                        real* out) const {
  const int bin_ms = static_cast<int>(bin_us_ / 1000);
  const int ring = 2 * bins_;
  const int k0 = ms0 / bin_ms;
  const int k1 = ms1 / bin_ms;
  real sum = 0.0f;
  int n = 0;
  for (int k = k0; k < k1; ++k) {
    const int idx = static_cast<int>((start_bin + k) % ring);
    if (have_[idx]) {
      sum += val_[idx];
      ++n;
    }
  }
  if (n == 0 || n * 2 < (k1 - k0)) return false;  // more hole than window
  *out = sum / static_cast<real>(n);
  return true;
}

void SubcarrierSecondReader::readSecond(int64_t start_bin) {
  const int sidx = static_cast<int>(start_bin % (2 * bins_));
  const int64_t edge = have_[sidx] ? stamp_[sidx] : origin_us_ + start_bin * bin_us_;
  ++diag_.seconds;

  real w_on = 0.0f, w_one = 0.0f, w_mark = 0.0f, w_off = 0.0f;
  const bool ok = windowMean(start_bin, kOnMs0, kOnMs1, &w_on) &&
                  windowMean(start_bin, kOneMs0, kOneMs1, &w_one) &&
                  windowMean(start_bin, kMarkMs0, kMarkMs1, &w_mark) &&
                  windowMean(start_bin, kOffMs0, kOffMs1, &w_off);
  const real mid = off_ + 0.5f * (on_ - off_);
  if (!ok || on_ <= off_ || w_on <= mid || w_off >= mid) {
    ++diag_.unreadable;
    push(-1, edge);
    return;
  }
  push(w_one > mid ? (w_mark > mid ? 770 : 470) : 170, edge);
}

}  // namespace airtime
