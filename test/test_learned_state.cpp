#include <cstdint>
#include <cstring>

#include "airtime/learned_state.h"
#include "test_framework.h"

using namespace airtime;

namespace {
constexpr int64_t kMs = 1000;

StationBiasTable makeTable() {
  StationBiasTable t;
  t.seed(0xA920, 62 * kMs, 9);
  t.seed(0x6E47, 216 * kMs, 5);
  t.seed(0x33CB, -40 * kMs, 4);   // an EARLY station: the sign must survive
  return t;
}
}  // namespace

AT_TEST(learned_station_bias_round_trips) {
  const StationBiasTable src = makeTable();
  uint8_t blob[kStationBiasBlobMax];
  const std::size_t n = encodeStationBias(src, blob, sizeof(blob));
  AT_CHECK(n > 0);

  StationBiasTable dst;
  AT_CHECK(decodeStationBias(blob, n, &dst));
  AT_CHECK_EQ(dst.count(), src.count());
  AT_CHECK_EQ(dst.correction(0xA920), 62 * kMs);
  AT_CHECK_EQ(dst.correction(0x6E47), 216 * kMs);
  AT_CHECK_EQ(dst.correction(0x33CB), -40 * kMs);   // sign preserved
}

// Flash can hand back anything: an older build's format, a half-finished
// write, another device's blob. A decoder that trusts it invents a station
// bias and poisons a working clock — far worse than simply relearning.
AT_TEST(learned_state_rejects_anything_it_cannot_verify) {
  uint8_t blob[kStationBiasBlobMax];
  const std::size_t n = encodeStationBias(makeTable(), blob, sizeof(blob));

  StationBiasTable dst;
  AT_CHECK(!decodeStationBias(blob, 0, &dst));          // empty
  AT_CHECK(!decodeStationBias(blob, 1, &dst));          // header only
  AT_CHECK(!decodeStationBias(blob, n - 3, &dst));      // truncated mid-record
  AT_CHECK_EQ(dst.count(), 0u);                          // ...and nothing applied

  uint8_t bad[kStationBiasBlobMax];
  std::memcpy(bad, blob, n);
  bad[0] = 99;                                           // unknown version
  AT_CHECK(!decodeStationBias(bad, n, &dst));
  AT_CHECK_EQ(dst.count(), 0u);

  std::memcpy(bad, blob, n);
  bad[1] = 200;                                          // impossible count
  AT_CHECK(!decodeStationBias(bad, n, &dst));
  AT_CHECK_EQ(dst.count(), 0u);
}

// A stored bias beyond the sane limit must be dropped by the same rule that
// governs live measurement, not waved through because it came from flash.
AT_TEST(learned_state_applies_policy_to_restored_values) {
  uint8_t blob[kStationBiasBlobMax];
  StationBiasTable src;
  src.seed(0x1001, 60 * kMs, 8);
  std::size_t n = encodeStationBias(src, blob, sizeof(blob));

  // Hand-edit the bias field to 9 seconds (record starts at byte 2; bias is
  // bytes 4..7, little-endian milliseconds).
  const int32_t nine_s_ms = 9000;
  blob[4] = (uint8_t)(nine_s_ms & 0xFF);
  blob[5] = (uint8_t)((nine_s_ms >> 8) & 0xFF);
  blob[6] = 0;
  blob[7] = 0;

  StationBiasTable dst;
  AT_CHECK(decodeStationBias(blob, n, &dst));
  AT_CHECK_EQ(dst.correction(0x1001), 0);     // refused, exactly as if measured
}

AT_TEST(learned_band_stats_round_trip) {
  Scheduler src;
  src.start(0);
  src.tick(2 * 60 * 1000000);       // step to 10 MHz
  src.onWwvMarker(14.5f);           // credit it

  uint8_t blob[kBandStatsBlobMax];
  const std::size_t n = encodeBandStats(src, blob, sizeof(blob));
  AT_CHECK(n > 0);

  Scheduler dst;
  AT_CHECK(decodeBandStats(blob, n, &dst));
  AT_CHECK_EQ(dst.bandStats(1).successes, 1);
  AT_CHECK_NEAR(dst.bandStats(1).best_snr, 14.5, 0.01);
  AT_CHECK_EQ(dst.preferredBandIndex(), 1u);   // opens on the proven band
}

// Bands are matched by frequency, not slot. A build that reorders or extends
// the rotation must not credit 5 MHz with what 15 MHz earned.
AT_TEST(learned_band_stats_match_by_frequency_not_index) {
  Scheduler src;
  const int32_t original[] = {5000, 10000, 15000};
  src.setBands(original, 3);
  src.seedBandStats(15000, 7, 12.0f);

  uint8_t blob[kBandStatsBlobMax];
  const std::size_t n = encodeBandStats(src, blob, sizeof(blob));

  Scheduler dst;
  const int32_t reordered[] = {15000, 10000, 5000};   // a later build's order
  dst.setBands(reordered, 3);
  AT_CHECK(decodeBandStats(blob, n, &dst));

  AT_CHECK_EQ(dst.bandStats(0).successes, 7);   // 15 MHz, now at slot 0
  AT_CHECK_EQ(dst.bandStats(2).successes, 0);   // 5 MHz earned nothing
}

// A band this radio no longer rotates through is simply ignored.
AT_TEST(learned_band_stats_ignore_unknown_frequencies) {
  Scheduler src;
  const int32_t exotic[] = {2500, 20000};
  src.setBands(exotic, 2);
  src.seedBandStats(2500, 5, 9.0f);

  uint8_t blob[kBandStatsBlobMax];
  const std::size_t n = encodeBandStats(src, blob, sizeof(blob));

  Scheduler dst;   // defaults: 5/10/15 MHz — no overlap at all
  AT_CHECK(decodeBandStats(blob, n, &dst));
  for (std::size_t i = 0; i < dst.bandCount(); ++i) {
    AT_CHECK_EQ(dst.bandStats(i).successes, 0);
  }
}

// Too small a buffer must fail loudly rather than write a short blob that
// would decode as truncated forever after.
AT_TEST(learned_state_refuses_a_short_buffer) {
  uint8_t tiny[4];
  AT_CHECK_EQ(encodeStationBias(makeTable(), tiny, sizeof(tiny)), 0u);
  Scheduler s;
  AT_CHECK_EQ(encodeBandStats(s, tiny, sizeof(tiny)), 0u);
}
