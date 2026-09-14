#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "airtime/wwv_subcarrier.h"
#include "airtime/wwv_timecode.h"
#include "test_framework.h"

using airtime::real;
using airtime::SubcarrierSecondReader;
using airtime::TcSymbol;
using airtime::WwvTimecodeDecoder;

namespace {

// Deterministic noise: xorshift64*, exponential by inversion. The power in one
// detector bin of Gaussian noise is exponentially distributed — jumpy enough
// that a running mean and a fixed ratio misbehave, which the constant-noise
// fakes could never show.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  double uniform() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return static_cast<double>((s * 2685821657736338717ULL) >> 11) *
           (1.0 / 9007199254740992.0);
  }
  double exponential(double mean) {
    double u = uniform();
    if (u < 1e-12) u = 1e-12;
    return -std::log(u) * mean;
  }
};

constexpr int64_t kS = 1000000;
constexpr int64_t kLeadUs = 30000;          // NIST: the code pulse opens 30 ms in
constexpr int64_t kStartEpochS = 1789400000;  // September 2026

// One listening session: WWV's subcarrier as the sampler sees it, fed to the
// reader, and the reader's seconds fed to the decoder — the app's chain
// without the app.
struct Session {
  SubcarrierSecondReader reader;
  WwvTimecodeDecoder decoder;
  Rng rng{12345};
  double tone = 0.0;       // power added while the pulse is on (0: no station)
  double noise = 1.0;      // mean noise power per block
  bool random = true;      // exponential noise; false: constant, like the old fakes
  int64_t block_us = 50000;
  int64_t mono_offset_us = 7777777;          // mono = true + offset: no free alignment
  int64_t t_us = kStartEpochS * kS + 23 * kS + 12345;  // true UTC, deliberately mid-minute
  TcSymbol frame[60] = {};
  int64_t frame_minute = -1;
  int confirmed = 0;
  int64_t confirmed_err_us = 0;

  real powerAt(int64_t true_us) {
    const int64_t s = true_us / kS;
    const int64_t within = true_us % kS;
    const int64_t minute = (s / 60) * 60;
    if (minute != frame_minute) {
      airtime::encodeFrame(airtime::wwvTimeFromEpochS(minute), frame);
      frame_minute = minute;
    }
    int64_t width = 0;
    switch (frame[s - minute]) {
      case TcSymbol::Zero: width = 170000; break;
      case TcSymbol::One: width = 470000; break;
      case TcSymbol::Marker: width = 770000; break;
      default: break;
    }
    const bool on = tone > 0.0 && within >= kLeadUs && within < kLeadUs + width;
    const double n = random ? rng.exponential(noise) : noise;
    return static_cast<real>(n + (on ? tone : 0.0));
  }

  void run(int64_t dur_us) {
    const int64_t end = t_us + dur_us;
    for (; t_us < end; t_us += block_us) {
      const uint32_t locks = reader.diag().locks;
      reader.process(t_us + mono_offset_us, powerAt(t_us));
      if (reader.diag().locks != locks) decoder.reset();  // as the app does
      int64_t ms = 0, edge = 0;
      while (reader.next(&ms, &edge)) {
        if (!decoder.onSecond(ms, edge)) continue;
        ++confirmed;
        const int64_t claimed =
            airtime::wwvTimeToEpochS(decoder.time()) * kS + kLeadUs;
        const int64_t actual = decoder.frameStartUs() - mono_offset_us;
        confirmed_err_us = claimed - actual;
      }
    }
  }
};

}  // namespace

AT_TEST(subcarrier_reads_a_strong_signal_and_the_decoder_confirms) {
  Session s;
  s.tone = 20.0;
  s.run(5 * 60 * kS);
  AT_CHECK(s.reader.diag().locked);
  AT_CHECK(s.confirmed >= 1);
  AT_CHECK(std::llabs(s.confirmed_err_us) < 120000);
  AT_CHECK(s.reader.diag().unreadable * 20 < s.reader.diag().seconds);
}

// The field failure at its own numbers: constant levels, pulse ~5x the floor.
// The edge detector this replaces counted one pulse in forty minutes here.
AT_TEST(subcarrier_does_not_lock_out_at_field_levels) {
  Session s;
  s.random = false;
  s.noise = 6.0e-6;
  s.tone = 2.3e-5;   // on-level 2.9e-5, the code[] peak the radio reported
  s.run(4 * 60 * kS);
  AT_CHECK(s.reader.diag().seconds > 200);
  AT_CHECK(s.reader.diag().unreadable < 5);
  AT_CHECK(s.confirmed >= 1);
}

// Weak and noisy: the pulse adds only 4x the mean noise to each block, in
// exponential noise. Folding and windowed reads are what make this decodable.
AT_TEST(subcarrier_decodes_a_weak_noisy_signal) {
  Session s;
  s.tone = 4.0;
  s.run(12 * 60 * kS);
  AT_CHECK(s.reader.diag().locked);
  AT_CHECK(s.confirmed >= 1);
  AT_CHECK(std::llabs(s.confirmed_err_us) < 120000);
}

// An hour of noise and nothing else. The reader may lock on a fluke; no time
// may ever come out of it.
AT_TEST(subcarrier_noise_alone_never_produces_a_time) {
  Session s;
  s.tone = 0.0;
  s.run(60 * 60 * kS);
  AT_CHECK_EQ(s.confirmed, 0);
}

// The device's sampler does not deliver exactly 50 ms blocks. Slightly short
// ones mean some bins get two blocks; the read must not drift or slip.
AT_TEST(subcarrier_tolerates_uneven_block_timing) {
  Session s;
  s.tone = 10.0;
  s.block_us = 49370;
  s.run(6 * 60 * kS);
  AT_CHECK(s.confirmed >= 1);
  AT_CHECK(std::llabs(s.confirmed_err_us) < 120000);
}

// A band change resets the reader; it must lock again and confirm again.
AT_TEST(subcarrier_relocks_after_a_reset) {
  Session s;
  s.tone = 20.0;
  s.run(4 * 60 * kS);
  const int first = s.confirmed;
  AT_CHECK(first >= 1);
  s.reader.reset();
  s.decoder.reset();
  s.run(4 * 60 * kS);
  AT_CHECK(s.reader.diag().locked);
  AT_CHECK(s.confirmed > first);
}
