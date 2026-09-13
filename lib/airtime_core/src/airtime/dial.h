#pragma once
//
// Which band table entry should hold a dial frequency.
//
// This is three lines of loop and one genuine trap, and the trap is why it
// lives here rather than in the firmware glue: it shipped once as a
// first-match search, was caught only because someone traced 7047 kHz through
// the table by hand, and the glue layer has no tests to catch it next time.
// Host code can hold the real shipped table and prove the choice.
//
// ── The trap ────────────────────────────────────────────────────────────────
//
// Band tables overlap heavily. ats-mini's carries "ALL" (150–30000 kHz),
// "41M" (7000–9000) and "40M" (7000–7300), and 7047 kHz sits inside all
// three. First-match returns "ALL" for every net in the directory — and the
// caller (tuneToMemory) writes the requested MODE into whichever band it
// lands on, so tuning one CW net would have left the general-coverage band
// stuck in USB permanently.
//
// Narrowest span wins instead: the tightest band containing a frequency is
// the one somebody drew around it on purpose. Ties go to a band already in
// the wanted mode, so a net does not disturb an equally-good band's settings
// when it does not have to.

#include <cstddef>
#include <cstdint>

namespace airtime {

// One row of the caller's band table, reduced to what the choice needs.
// `mode` is opaque to this function — it is compared for the tie-break and
// returned to a caller who knows what the values mean (the firmware's
// FM/LSB/USB/AM constants; nothing here depends on them).
struct DialBandSpan {
  int32_t min_khz = 0;
  int32_t max_khz = 0;
  bool fm = false;        // FM and AM/SSB bands never substitute for each other
  uint8_t mode = 0;
};

// Index of the band that should hold `khz`, or -1 if none can — which is a
// table problem, not an operator problem, and the caller should say so rather
// than tune somewhere plausible and wrong.
int pickDialBand(const DialBandSpan* bands, std::size_t count,
                 int32_t khz, bool want_fm, uint8_t want_mode);

}  // namespace airtime
