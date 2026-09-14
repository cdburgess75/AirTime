#pragma once
//
// Reading the WWV/WWVH 100 Hz time code by its timing, not by a threshold.
//
// ── Why not an edge detector ────────────────────────────────────────────────
//
// The first front end was the minute-marker detector pointed at 100 Hz: a
// pulse "starts" when a block beats a running noise estimate by a ratio. In
// the field it failed in a specific way. The subcarrier is on for 17-77 % of
// every second, so whenever one pulse was missed its energy went into the
// noise average, the threshold rose, and the next pulse was missed too — one
// miss locked it out for good (tone ~5x noise: one pulse counted in forty
// minutes). No fixed floor fixes that, and no fixed number is right for every
// radio, antenna and band anyway.
//
// ── What this does instead ──────────────────────────────────────────────────
//
// The code has the same shape in every second: the pulse opens at the second
// (after a 30 ms lead), every symbol is ON for its first 170 ms, and every
// symbol is OFF for the last 200 ms. So:
//
//   1. Fold. Average block power by position within the second, over many
//      seconds. Noise averages flat; the pulse piles up where it opens.
//   2. Lock. The position whose always-on window most exceeds its always-off
//      window 800 ms later is the second boundary. It must hold for several
//      seconds before it counts, and is only moved for a clearly better one.
//   3. Read. Each second is judged against levels taken from the fold itself —
//      the always-on window is ON, the always-off window is OFF — and the
//      250-450 ms and 550-750 ms windows decide zero / one / marker against
//      the midpoint. Nothing here is an absolute power.
//
// A second whose always-on window is not above the midpoint, or whose
// always-off window is not below it, is reported unreadable (-1), never
// guessed. The decoder downstream still requires two frames in whole-minute
// lockstep before it believes anything, so a false lock on noise costs
// listening time, not a wrong clock.

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

struct SubcarrierReaderConfig {
  int64_t block_us = 50000;    // the sampler's sub block; the app sets it from AppConfig
  real fold_alpha = 0.125f;    // weight of each new second in the fold (~8 s memory)
  real lock_contrast = 1.8f;   // always-on over always-off, in the fold, to lock at all
  real relock_margin = 1.25f;  // a different position must beat the held one by this...
  int lock_hold_s = 4;         // ...for this many seconds running (so must a first lock)
};

struct SubcarrierReaderDiag {
  uint32_t blocks = 0;
  uint32_t seconds = 0;      // seconds read after lock, readable or not
  uint32_t unreadable = 0;
  uint32_t locks = 0;        // phase locks taken, first and later
  bool locked = false;
  int phase_bin = -1;
  real contrast = 0.0f;      // best fold contrast at the last check
  real on_level = 0.0f;
  real off_level = 0.0f;
  real max_power = 0.0f;
};

class SubcarrierSecondReader {
 public:
  explicit SubcarrierSecondReader(const SubcarrierReaderConfig& cfg = {});

  // One sub block: its monotonic timestamp and its power in the 100 Hz bin.
  void process(int64_t mono_us, real power);

  // The next second read, oldest first. `pulse_ms` is 170, 470 or 770 (the
  // decoder's nominal widths) or -1 for unreadable; `edge_us` is the monotonic
  // time the second's pulse opened. False when none is waiting.
  bool next(int64_t* pulse_ms, int64_t* edge_us);

  // Forget the signal (a band change). Counters and max_power are kept: they
  // describe the whole listening history, and the serial log reads them.
  void reset();

  const SubcarrierReaderDiag& diag() const { return diag_; }

  static constexpr int kMaxBins = 64;
  static constexpr int kQueue = 64;

 private:
  void finalizeBin(int64_t abs_bin, bool have, real value, int64_t stamp);
  void evaluatePhase();
  void readSecond(int64_t start_bin);
  bool windowMean(int64_t start_bin, int ms0, int ms1, real* out) const;
  real foldWindow(int phase, int ms0, int ms1) const;
  real foldContrast(int phase) const;
  void push(int64_t pulse_ms, int64_t edge_us);

  SubcarrierReaderConfig cfg_;
  int bins_ = 20;
  int64_t bin_us_ = 50000;

  bool have_origin_ = false;
  int64_t origin_us_ = 0;
  int64_t cur_bin_ = -1;
  real cur_sum_ = 0.0f;
  int cur_n_ = 0;
  int64_t cur_stamp_ = 0;

  // The last two seconds of bins, indexed by absolute bin modulo 2 * bins_.
  real val_[2 * kMaxBins] = {};
  bool have_[2 * kMaxBins] = {};
  int64_t stamp_[2 * kMaxBins] = {};

  real fold_[kMaxBins] = {};
  bool fold_have_[kMaxBins] = {};

  int phase_ = -1;
  int cand_phase_ = -1;
  int cand_run_ = 0;
  real on_ = 0.0f;
  real off_ = 0.0f;

  struct Out {
    int64_t pulse_ms;
    int64_t edge_us;
  };
  Out queue_[kQueue] = {};
  int q_head_ = 0;
  int q_len_ = 0;

  SubcarrierReaderDiag diag_;
};

}  // namespace airtime
