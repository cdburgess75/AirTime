#include "morse.h"

#include <algorithm>

namespace airtime {

namespace {

// ITU Morse, as (element count, bits MSB-first, character). A dah is 1, a dit
// is 0, so 'A' (.-) is count 2, bits 0b01.
struct MorseEntry {
  uint8_t count;
  uint8_t bits;
  char ch;
};

const MorseEntry kTable[] = {
    {2, 0x1, 'A'}, {4, 0x8, 'B'}, {4, 0xA, 'C'}, {3, 0x4, 'D'},
    {1, 0x0, 'E'}, {4, 0x2, 'F'}, {3, 0x6, 'G'}, {4, 0x0, 'H'},
    {2, 0x0, 'I'}, {4, 0x7, 'J'}, {3, 0x5, 'K'}, {4, 0x4, 'L'},
    {2, 0x3, 'M'}, {2, 0x2, 'N'}, {3, 0x7, 'O'}, {4, 0x6, 'P'},
    {4, 0xD, 'Q'}, {3, 0x2, 'R'}, {3, 0x0, 'S'}, {1, 0x1, 'T'},
    {3, 0x1, 'U'}, {4, 0x1, 'V'}, {3, 0x3, 'W'}, {4, 0x9, 'X'},
    {4, 0xB, 'Y'}, {4, 0xC, 'Z'},
    {5, 0x1F, '0'}, {5, 0x0F, '1'}, {5, 0x07, '2'}, {5, 0x03, '3'},
    {5, 0x01, '4'}, {5, 0x00, '5'}, {5, 0x10, '6'}, {5, 0x18, '7'},
    {5, 0x1C, '8'}, {5, 0x1E, '9'},
    // The punctuation that actually turns up in a bulletin.
    {6, 0x15, '?'},   // ..--..
    {6, 0x2A, '.'},   // .-.-.-
    {6, 0x33, ','},   // --..--
    {6, 0x38, ':'},   // ---...
    {5, 0x11, '/'},   // -..-.
    {6, 0x21, '-'},   // -....-
    {5, 0x0D, '+'},   // .-.-.   (AR, also sent as +)
    {6, 0x0C, '"'},   // .-..-.
    {6, 0x1E, '@'},   // .--.-.
    {5, 0x09, '='},   // -...-   (BT, the paragraph break W1AW leans on)
};

}  // namespace

char morseLookup(uint8_t bits, int count) {
  for (const MorseEntry& e : kTable) {
    if (e.count == count && e.bits == bits) return e.ch;
  }
  return 0;
}

MorseDecoder::MorseDecoder(const MorseConfig& cfg) : cfg_(cfg) { reset(); }

void MorseDecoder::trackNoise(real power) {
  const real a = power < noise_ ? cfg_.noise_alpha_down : cfg_.noise_alpha_up;
  noise_ += a * (power - noise_);
}

void MorseDecoder::reset() {
  in_tone_ = false;
  // Start the floor so that the on-threshold sits exactly at min_power. Seeding
  // it from the first sample instead is a trap: tune into a transmission that
  // is already keyed and the very first sample is a TONE, which puts the
  // threshold above the signal and leaves the decoder permanently deaf. Found
  // exactly that way — the first test to send without leading silence decoded
  // nothing at all.
  noise_ = cfg_.min_power / cfg_.on_ratio;
  edge_us_ = 0;
  unit_us_ = cfg_.unit_us_init;
  have_unit_ = false;
  count_ = 0;
  pending_space_ = false;
  st_ = MorseStatus{};
}

void MorseDecoder::learnUnit(int64_t mark_us) {
  // The estimator has to BOOTSTRAP, not merely track, and that is the hard
  // part: classifying an element requires the unit, and measuring the unit
  // requires classified elements. Seeded at 20 WPM and handed 8 WPM traffic,
  // a naive "is this mark short or long" rule reads every dit as a dah, halves
  // its estimate to stay consistent, and locks there permanently — decoding
  // fluent nonsense forever. Found exactly that way.
  //
  // The escape is that a dit is the SHORTEST thing on the air. So any mark
  // shorter than the current estimate is adopted outright: one dit corrects an
  // estimate that is too slow, immediately. The other two rules refine rather
  // than rescue.
  if (mark_us < cfg_.unit_us_min / 2 || mark_us > cfg_.unit_us_max * 4) return;

  if (!have_unit_) {
    unit_us_ = mark_us;
    have_unit_ = true;
  } else if (mark_us < unit_us_) {
    unit_us_ = mark_us;            // shorter than anything seen: this is the dit
  } else if (mark_us < unit_us_ * 2) {
    // Another dit, slightly longer: average it in.
    unit_us_ += static_cast<int64_t>(cfg_.unit_alpha *
                                     static_cast<real>(mark_us - unit_us_));
  } else {
    // A dah. It says the unit is a third of this — a weak claim on its own, so
    // applied slowly, but it is what lets the estimate FOLLOW a sender who
    // slows down, when no short mark ever arrives to adopt.
    unit_us_ += static_cast<int64_t>(0.05f *
                                     static_cast<real>(mark_us / 3 - unit_us_));
  }

  if (unit_us_ < cfg_.unit_us_min) unit_us_ = cfg_.unit_us_min;
  if (unit_us_ > cfg_.unit_us_max) unit_us_ = cfg_.unit_us_max;
}

bool MorseDecoder::emit(char* out) {
  if (count_ == 0) return false;
  // Judge every element now, against the unit as it is understood at the END of
  // the character — see the note on marks_.
  uint8_t bits = 0;
  for (int i = 0; i < count_; ++i) {
    const bool dah = marks_[i] >= unit_us_ * 2;
    bits = static_cast<uint8_t>((bits << 1) | (dah ? 1 : 0));
  }
  const char c = morseLookup(bits, count_);
  count_ = 0;
  if (c == 0) {
    ++st_.unknown;
    return false;
  }
  ++st_.chars;
  if (out != nullptr) *out = c;
  return true;
}

bool MorseDecoder::process(int64_t mono_us, real power, char* out) {
  bool emitted = false;

  // A word gap owes two characters: the one it terminated, then the space.
  // Deliver the space before looking at any more audio, so the text comes out
  // in the order it was sent.
  if (pending_space_) {
    pending_space_ = false;
    if (out != nullptr) *out = ' ';
    ++st_.chars;
    return true;
  }

  if (!in_tone_) {
    // Track the floor only while the key is up, so a long dah cannot inflate
    // it — and asymmetrically, so it falls to a quiet band quickly but is
    // dragged upward only slowly by whatever is not silence.
    trackNoise(power);

    const real on_th = std::max(noise_ * cfg_.on_ratio, cfg_.min_power);
    if (power > on_th) {
      // Key down. The silence that just ended tells us where we are in the
      // message: within a character, between characters, or between words.
      const int64_t gap = mono_us - edge_us_;
      if (count_ > 0) {
        if (gap >= unit_us_ * 5) {
          emitted = emit(out);       // word gap: character now, space next call
          pending_space_ = st_.chars > 0;
        } else if (gap >= unit_us_ * 2) {
          emitted = emit(out);       // letter gap
        }
        // Shorter than that is the gap between elements of one character:
        // nothing to do, the pattern keeps building.
      }
      in_tone_ = true;
      edge_us_ = mono_us;
    } else if (count_ > 0 && (mono_us - edge_us_) > cfg_.idle_flush_us) {
      emitted = emit(out);           // transmission stopped mid-character
    }
  } else {
    const real off_th = std::max(noise_ * cfg_.off_ratio, cfg_.min_power * 0.5f);
    if (power < off_th) {
      const int64_t mark = mono_us - edge_us_;
      in_tone_ = false;
      edge_us_ = mono_us;

      if (mark > cfg_.max_mark_us) {
        // A stuck carrier or a tuning whistle, not an element. Drop whatever
        // was building rather than recording a forty-unit dah.
        count_ = 0;
      } else {
        learnUnit(mark);
        if (count_ < 7) {
          marks_[count_++] = mark;
        } else {
          // Longer than any ITU character: prosign or garbage. Let it lapse.
          count_ = 0;
          ++st_.unknown;
        }
      }
      trackNoise(power);   // resume from this first silent sample
    }
  }

  st_.key_down = in_tone_;
  st_.elements = count_;
  return emitted;
}

bool MorseDecoder::flush(char* out) { return emit(out); }

MorseStatus MorseDecoder::status() const {
  MorseStatus s = st_;
  // 1200 / unit_ms, the standard PARIS definition.
  s.wpm = have_unit_ && unit_us_ > 0
              ? static_cast<int>((1200000 + unit_us_ / 2) / unit_us_)
              : 0;
  return s;
}

void MorseTextBuffer::clear() {
  buf_[0] = '\0';
  len_ = 0;
}

void MorseTextBuffer::push(char c) {
  if (c == '\0') return;
  // A word gap at the very start, or a second one in a row, carries no
  // information and costs a column of a very small screen.
  if (c == ' ' && (len_ == 0 || buf_[len_ - 1] == ' ')) return;

  if (len_ == kMax) {
    // Full: the oldest character leaves. kMax is 96 and characters arrive at
    // a few per second even at 40 WPM, so shifting beats the bookkeeping a
    // ring buffer would need to hand out a contiguous string.
    for (std::size_t i = 1; i < kMax; ++i) buf_[i - 1] = buf_[i];
    len_ = kMax - 1;
  }
  buf_[len_++] = c;
  buf_[len_] = '\0';
}

const char* MorseTextBuffer::tail(std::size_t n) const {
  return (len_ > n) ? (buf_ + (len_ - n)) : buf_;
}

}  // namespace airtime
