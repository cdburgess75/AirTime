#include <cstdint>

#include "test_framework.h"
#include "airtime/wwv_marker.h"

using airtime::real;
using airtime::WwvMarker;
using airtime::WwvMarkerDetector;

namespace {
// Feed a run of `count` blocks of constant power starting at block index `from`,
// 100 ms apart. Returns true if a marker was emitted during the run.
struct Feeder {
  WwvMarkerDetector det;
  WwvMarker marker{};
  bool got = false;
  void feed(int block_index, real power) {
    if (det.process(static_cast<int64_t>(block_index) * 100000, power, &marker))
      got = true;
  }
};
}  // namespace

// An 800 ms tone flanked by silence is accepted; the leading edge is timestamped.
AT_TEST(wwv_detects_800ms_marker) {
  Feeder f;
  for (int i = 0; i < 10; ++i) f.feed(i, 0.001f);      // silence, blocks 0..9
  for (int i = 10; i < 18; ++i) f.feed(i, 1.0f);       // tone, blocks 10..17 (800 ms)
  f.feed(18, 0.001f);                                  // falling edge at 1800 ms
  AT_CHECK(f.got);
  AT_CHECK_EQ(f.marker.leading_edge_us, 1000000LL);
  AT_CHECK_EQ(f.marker.duration_us, 800000LL);
  AT_CHECK(f.marker.peak_power > 0.9f);
}

// A 200 ms blip (like a seconds tick) is below the duration gate => rejected.
AT_TEST(wwv_rejects_short_blip) {
  Feeder f;
  for (int i = 0; i < 10; ++i) f.feed(i, 0.001f);
  for (int i = 10; i < 12; ++i) f.feed(i, 1.0f);  // 200 ms
  f.feed(12, 0.001f);
  AT_CHECK(!f.got);
}

// A 1500 ms tone (e.g. a continuous standard tone leaking through) is above the
// gate => rejected.
AT_TEST(wwv_rejects_long_tone) {
  Feeder f;
  for (int i = 0; i < 10; ++i) f.feed(i, 0.001f);
  for (int i = 10; i < 25; ++i) f.feed(i, 1.0f);  // 1500 ms
  f.feed(25, 0.001f);
  AT_CHECK(!f.got);
}

// A burst of exactly marker length, but below the absolute power floor, is
// rejected — the ratio test alone must not be able to promote noise.
AT_TEST(wwv_ignores_subfloor_burst) {
  Feeder f;
  for (int i = 0; i < 10; ++i) f.feed(i, 1e-5f);    // quiet
  for (int i = 10; i < 18; ++i) f.feed(i, 8e-5f);   // 800 ms, still under min_power
  f.feed(18, 1e-5f);
  AT_CHECK(!f.got);
  AT_CHECK(!f.det.inTone());
}

// Levels as actually measured on hardware (Milestone 0 §6) must detect.
AT_TEST(wwv_detects_at_measured_levels) {
  Feeder f;
  for (int i = 0; i < 10; ++i) f.feed(i, 7.7e-5f);   // measured in-bin noise
  for (int i = 10; i < 18; ++i) f.feed(i, 1.9e-3f);  // measured tone level
  f.feed(18, 7.7e-5f);
  AT_CHECK(f.got);
  AT_CHECK_EQ(f.marker.duration_us, 800000LL);
}
