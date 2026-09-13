#include <cstdint>
#include <cstring>
#include <string>

#include "airtime/morse.h"
#include "test_framework.h"

using airtime::MorseConfig;
using airtime::MorseDecoder;
using airtime::morseLookup;
using airtime::real;

namespace {

// A sending key. Feeds the decoder blocks of tone power exactly as the Goertzel
// would, at realistic measured levels, and collects whatever comes out.
class Key {
 public:
  explicit Key(int wpm, real tone = 1.9e-3f, real noise = 7.7e-5f)
      : unit_us_(1200000 / wpm), tone_(tone), noise_(noise) {}

  // 5 ms blocks: what CW needs (see morse.h — 20 ms blocks cap usable speed
  // near 15 WPM because a 40 WPM dit is only 30 ms long).
  void run(int64_t dur_us, real power) {
    for (int64_t t = 0; t < dur_us; t += kBlock) {
      char c = 0;
      if (dec.process(now_, power, &c)) text_ += c;
      now_ += kBlock;
    }
  }

  void send(const char* morse) {  // ".-" style, one character
    for (const char* p = morse; *p; ++p) {
      run(*p == '-' ? unit_us_ * 3 : unit_us_, tone_);
      if (p[1]) run(unit_us_, noise_);          // gap between elements
    }
  }
  void letterGap() { run(unit_us_ * 3, noise_); }
  void wordGap() { run(unit_us_ * 7, noise_); }
  void quiet(int64_t us) { run(us, noise_); }

  std::string finish() {
    quiet(unit_us_ * 10);
    char c = 0;
    if (dec.flush(&c)) text_ += c;
    return text_;
  }

  MorseDecoder dec;
  const std::string& text() const { return text_; }

 private:
  static constexpr int64_t kBlock = 5000;
  int64_t unit_us_;
  real tone_, noise_;
  int64_t now_ = 0;
  std::string text_;
};

}  // namespace

AT_TEST(morse_table_covers_the_alphabet) {
  AT_CHECK_EQ(morseLookup(0x1, 2), 'A');   // .-
  AT_CHECK_EQ(morseLookup(0x0, 1), 'E');   // .
  AT_CHECK_EQ(morseLookup(0x1, 1), 'T');   // -
  AT_CHECK_EQ(morseLookup(0x7, 3), 'O');   // ---
  AT_CHECK_EQ(morseLookup(0x0, 4), 'H');   // ....
  AT_CHECK_EQ(morseLookup(0x00, 5), '5');  // .....
  AT_CHECK_EQ(morseLookup(0x1F, 5), '0');  // -----
  AT_CHECK_EQ(morseLookup(0x09, 5), '=');  // -...-  (BT, the W1AW break)
  AT_CHECK_EQ(morseLookup(0x3F, 6), 0);    // no such character
}

// The canonical word, at the speed its timing defines.
AT_TEST(morse_decodes_paris_at_20wpm) {
  Key k(20);
  k.send(".--.");  k.letterGap();   // P
  k.send(".-");    k.letterGap();   // A
  k.send(".-.");   k.letterGap();   // R
  k.send("..");    k.letterGap();   // I
  k.send("...");   k.letterGap();   // S
  AT_CHECK(k.finish() == "PARIS");
}

// Word gaps must survive as spaces — a bulletin is unreadable without them.
AT_TEST(morse_separates_words) {
  Key k(18);
  k.send("-.-."); k.letterGap(); k.send("--.-");   // CQ
  k.wordGap();
  k.send(".--"); k.letterGap(); k.send("."); k.letterGap(); k.send("...-");  // WE
  const std::string got = k.finish();
  AT_CHECK(got.find("CQ") != std::string::npos);
  AT_CHECK(got.find(' ') != std::string::npos);
}

// Speed is measured, not configured: an operator should not have to tell the
// decoder how fast W1AW is sending, and W1AW changes speed between sessions.
AT_TEST(morse_measures_the_senders_speed) {
  for (int wpm : {8, 15, 25, 35}) {
    Key k(wpm);
    for (int i = 0; i < 6; ++i) {           // a few characters to lock on
      k.send("-.-."); k.letterGap();
      k.send("--.-"); k.letterGap();
    }
    k.finish();
    const int measured = k.dec.status().wpm;
    AT_CHECK(measured >= wpm - 3 && measured <= wpm + 3);
  }
}

// It must decode at a speed it was never told about, starting from a default
// that is wrong in both directions.
AT_TEST(morse_decodes_far_from_the_default_speed) {
  MorseConfig cfg;   // unit_us_init is 20 WPM
  for (int wpm : {7, 32}) {
    Key k(wpm);
    // The first character may be lost while the estimate is still the default;
    // that is honest behaviour, so lead in before asserting.
    k.send("."); k.letterGap();
    k.send("-"); k.letterGap();
    k.send("...");  k.letterGap();   // S
    k.send("---");  k.letterGap();   // O
    k.send("...");  k.letterGap();   // S
    const std::string got = k.finish();
    AT_CHECK(got.find("SOS") != std::string::npos);
  }
}

// A stuck carrier or a tuning whistle is not a forty-unit dah. It must abandon
// the character rather than emitting nonsense, and recover immediately after.
AT_TEST(morse_survives_a_stuck_carrier) {
  Key k(20);
  k.send("...");  k.letterGap();          // S, decoded normally
  k.run(3000000, 1.9e-3f);                // 3 s of carrier
  k.quiet(600000);
  k.send("---");  k.letterGap();          // O, after the interference
  const std::string got = k.finish();
  AT_CHECK(got.find('S') != std::string::npos);
  AT_CHECK(got.find('O') != std::string::npos);
}

// Silence must not manufacture characters, and the floor must not drift up
// into the signal during a long quiet spell.
AT_TEST(morse_emits_nothing_from_silence) {
  Key k(20);
  k.quiet(10000000);        // ten seconds of band noise
  AT_CHECK(k.finish().empty());
  AT_CHECK_EQ(k.dec.status().chars, 0u);
}

// A character interrupted by the end of transmission is still delivered.
AT_TEST(morse_flushes_a_trailing_character) {
  Key k(20);
  k.send("...");            // S, with no letter gap after it
  char c = 0;
  k.quiet(1000000);         // idle_flush_us worth of silence
  const std::string got = k.text().empty() ? std::string() : k.text();
  AT_CHECK(!got.empty() || k.dec.flush(&c));
}
