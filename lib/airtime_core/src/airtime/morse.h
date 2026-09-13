#pragma once
//
// CW (Morse) decoding from the same tone-power stream the WWV marker detector
// eats — W1AW code practice and bulletins, read off the screen.
//
// ── Why this is tractable when FT8 and WSPR are not ─────────────────────────
//
// CW is on/off keying of a single audio tone. That is precisely what this
// radio already does well: a Goertzel bin, an envelope, and a duration
// classifier (wwv_marker.h gates bursts at 700-900 ms; this one sorts them
// into dits and dahs). No FFT, no coherent integration over a hundred seconds,
// no 79-symbol Costas synchronisation, no sub-Hz resolution. The audio tap
// downstream of the DSP volume control, sampled at ~10 kHz, is enough — it was
// verified for detecting one tone, and one tone is all this needs.
//
// ── Timing, and the one hardware constraint that bites ──────────────────────
//
// Morse timing is defined in "units" of one dit: dah = 3, gap between elements
// = 1, between letters = 3, between words = 7. A unit is 1200/WPM ms, so W1AW's
// range of 5-40 WPM spans 240 ms down to 30 ms.
//
// The block size therefore matters more than it does for WWV. The 20 ms blocks
// used for minute markers give only 1.5 samples per dit at 40 WPM, which cannot
// resolve anything; CW wants ~5 ms blocks (4 samples per dit at 40 WPM, 48 at
// 5 WPM). At 10 kHz that is 50 samples per block, still ample selectivity for a
// ~700 Hz sidetone. The decoder itself is block-size agnostic — it works in
// microseconds — but feeding it 20 ms blocks caps usable speed near 15 WPM.
//
// ── Speed is measured, never configured ─────────────────────────────────────
//
// An operator should not have to tell a decoder how fast the other station is
// sending, and W1AW changes speed between sessions anyway. The unit length is
// estimated from the traffic itself and tracked continuously, which is also
// what makes the decoder tolerate a human fist rather than only machine keying.

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

struct MorseConfig {
  // Envelope thresholds, as multiples of the tracked noise floor. Hysteresis
  // for the same reason the WWV detector needs it: a bare threshold chatters
  // on the edges and shreds elements into fragments.
  real on_ratio = 4.0f;
  real off_ratio = 2.0f;
  real min_power = 2.0e-4f;   // absolute floor; matches the measured tap
  // Floor tracking is ASYMMETRIC: down fast, up slowly. A tuned-in CW signal
  // has the key down a large fraction of the time, so a symmetric filter drifts
  // up into the signal and goes deaf. Falling fast also means the estimate
  // recovers within one element gap after any disturbance.
  real noise_alpha_up = 0.01f;
  real noise_alpha_down = 0.3f;

  // Starting guess, replaced by measurement within a few characters.
  // 1200/20 ms = 20 WPM, mid-range for W1AW practice sessions.
  int64_t unit_us_init = 60000;
  // The speed range to believe. Outside this it is not CW, it is a carrier or
  // a burst of noise, and adapting to it would drag the estimate off real
  // traffic that resumes afterwards.
  int64_t unit_us_min = 25000;    // ~48 WPM
  int64_t unit_us_max = 250000;   // ~5 WPM
  // How fast the unit estimate follows the traffic. Slow enough to ignore one
  // clumsy element, quick enough to lock on within a word or two.
  real unit_alpha = 0.2f;

  // A key-down longer than this is not an element — a stuck carrier or a
  // tuning whistle. Abandon the character rather than recording a 40-unit dah.
  int64_t max_mark_us = 2000000;
  // Silence this long ends the message; anything held is flushed.
  int64_t idle_flush_us = 3000000;
};

// Live decoder state, for a display that should show what it is hearing even
// between characters.
struct MorseStatus {
  int wpm = 0;              // measured sending speed, 0 until known
  bool key_down = false;
  int elements = 0;         // elements buffered toward the current character
  uint32_t chars = 0;       // characters emitted since reset
  uint32_t unknown = 0;     // element patterns with no character (garbage/QRM)
};

class MorseDecoder {
 public:
  explicit MorseDecoder(const MorseConfig& cfg = MorseConfig{});

  // Feed one tone-power estimate. Returns true and sets *out when a character
  // is complete; a word gap emits ' ' as its own character.
  bool process(int64_t mono_us, real power, char* out);

  // No more audio is coming (band change, mode exit). Emits any character
  // still buffered.
  bool flush(char* out);

  void reset();

  MorseStatus status() const;
  real noiseFloor() const { return noise_; }

 private:
  bool emit(char* out);
  void trackNoise(real power);
  void learnUnit(int64_t mark_us);

  MorseConfig cfg_;
  bool in_tone_ = false;
  real noise_ = 0.0f;
  int64_t edge_us_ = 0;        // when the current mark or space began
  int64_t unit_us_ = 0;
  bool have_unit_ = false;

  // Element DURATIONS of the character being assembled, not yet classified.
  //
  // Classification is deferred to the end of the character on purpose. At the
  // start of a transmission the unit length is unknown, so the opening element
  // cannot be judged as it arrives — the first mark of "C" (-.-.) was read as a
  // dit and the letter came out as "F". By the time the character ends, its own
  // dits have taught the estimator the unit, and every element can be judged
  // against a figure that is actually correct.
  //
  // Seven elements covers every ITU character; longer runs are prosigns or
  // garbage and are dropped.
  int64_t marks_[7] = {};
  int count_ = 0;
  bool pending_space_ = false;

  MorseStatus st_;
};

// Decode an element pattern to a character. `bits` is MSB-first with `count`
// significant elements (1 = dah, 0 = dit). Returns 0 for no such character.
char morseLookup(uint8_t bits, int count);

// What the operator reads while it is happening.
//
// Deliberately not a scrollback. A pocket radio's panel is two short lines and
// the person holding it is reading live traffic, not reviewing a log; older
// text simply leaves. Keeping a transcript would mean deciding where to put it,
// how much to keep, and what to do when it fills — all questions worth
// answering for a logging feature and none of them worth answering for a
// display.
class MorseTextBuffer {
 public:
  static constexpr std::size_t kMax = 96;

  // Runs of spaces are collapsed and a leading space is dropped. The decoder
  // emits ' ' per word gap, and a pause in the traffic — which is most of a
  // code practice session — would otherwise push the last real word off the
  // left of the screen with nothing to show for it.
  void push(char c);
  void clear();

  const char* text() const { return buf_; }
  std::size_t size() const { return len_; }

  // The last `n` characters, for a line of that width.
  const char* tail(std::size_t n) const;

 private:
  char buf_[kMax + 1] = {};
  std::size_t len_ = 0;
};

}  // namespace airtime
