#pragma once
//
// Every clock source the radio has tried, and how far each has earned trust.
//
// The owner's rule: one source may set the clock, but it stays YELLOW until a
// DIFFERENT source confirms it, and only then is it GREEN. RED is a source that
// is not there (silent through whole dwells) or that is wrong (disagrees with a
// confirmed clock by more than a station bias can explain). A red source comes
// back the moment it delivers good time again — propagation and transmitters
// both change, and a rating is an observation, not a verdict.
//
// FM rows are keyed by frequency in SI4735 FM units (10 kHz: 8990 = 89.9 MHz)
// and carry the PI code once heard. WWV rows are keyed by kHz. Fixed size, no
// heap, persisted in the learned-state style: versioned and all-or-nothing.

#include <cstddef>
#include <cstdint>

namespace airtime {

enum class Rating : uint8_t { Unknown, Yellow, Green, Red };
enum class SourceKind : uint8_t { Fm, Wwv };

struct SourceRow {
  SourceKind kind = SourceKind::Fm;
  int32_t freq = 0;        // FM: 10 kHz units. WWV: kHz.
  uint16_t pi = 0;         // FM station identity once heard; 0 = not yet
  uint16_t heard = 0;      // clock-time deliveries
  uint16_t confirmed = 0;  // deliveries a DIFFERENT source agreed with
  uint8_t silent = 0;      // consecutive whole dwells with no clock time
  uint8_t tries = 0;       // whole dwells spent on it (saturates)
  bool wrong = false;      // its last word disagreed with a confirmed clock
  bool err_known = false;  // err_ms was measured against a confirmed clock
  int16_t err_ms = 0;      // that last disagreement, clamped to ±32 s
};

struct SourceTableConfig {
  int64_t agree_us = 400000;    // the voter's tolerance: within this, "agrees"
  int64_t wrong_us = 2000000;   // the station-bias cap: beyond this, wrong
  uint8_t silent_for_red = 2;   // whole dwells with nothing at all
};

class SourceTable {
 public:
  static constexpr std::size_t kMaxRows = 16;
  static constexpr std::size_t kBlobMax = 2 + kMaxRows * 16;

  explicit SourceTable(const SourceTableConfig& cfg = SourceTableConfig{})
      : cfg_(cfg) {}

  // The row for a source, created on first use. nullptr only when full.
  SourceRow* fm(int32_t khz10) { return rowFor(SourceKind::Fm, khz10); }
  SourceRow* wwv(int32_t khz) { return rowFor(SourceKind::Wwv, khz); }
  const SourceRow* find(SourceKind k, int32_t freq) const;
  SourceRow* findPi(uint16_t pi);

  // A source delivered a time. `err_us` is its disagreement with our clock;
  // `confirmable` says our clock is backed by a DIFFERENT source, so that the
  // disagreement actually measures this one. Without it the source has only
  // spoken, not been checked: it stays Yellow.
  void noteTime(SourceRow* r, int64_t err_us, bool confirmable);

  // A DIFFERENT source has just agreed with the time this one gave earlier.
  void noteConfirmed(SourceRow* r);

  // The source spoke, and a clock that can catch it (a hand-set one) says it is
  // wrong, even though that clock could not have confirmed it.
  void noteWrong(SourceRow* r, int64_t err_us);

  // A whole dwell on the source ended; `got_time` if it delivered any.
  void noteDwellEnd(SourceRow* r, bool got_time);

  static Rating rate(const SourceRow& r, const SourceTableConfig& cfg);
  Rating rating(const SourceRow& r) const { return rate(r, cfg_); }

  std::size_t count() const { return count_; }
  const SourceRow& at(std::size_t i) const { return rows_[i]; }
  void clear() { count_ = 0; }

  // "G 104.7", "Y WWV 15", "R 89.3", "? WWV 2.5" — never more than 12
  // characters, the width of the radio's menu zoom box.
  static char letter(Rating r);
  static void name(const SourceRow& r, Rating rt, char* buf, std::size_t n);

  std::size_t encode(void* buf, std::size_t cap) const;
  bool decode(const void* buf, std::size_t len);

 private:
  SourceRow* rowFor(SourceKind k, int32_t freq);

  SourceTableConfig cfg_;
  SourceRow rows_[kMaxRows];
  std::size_t count_ = 0;
};

}  // namespace airtime
