#include "learned_state.h"

#include <cstring>

namespace airtime {

namespace {

constexpr uint8_t kVersion = 1;

// Little-endian by hand rather than memcpy of a struct: the encoding must not
// change meaning if a compiler pads differently or a field is reordered, and
// this data is read back by whatever build happens to be flashed next.
void put16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v & 0xFF);
  *p++ = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void put32(uint8_t*& p, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  *p++ = static_cast<uint8_t>(u & 0xFF);
  *p++ = static_cast<uint8_t>((u >> 8) & 0xFF);
  *p++ = static_cast<uint8_t>((u >> 16) & 0xFF);
  *p++ = static_cast<uint8_t>((u >> 24) & 0xFF);
}
uint16_t get16(const uint8_t*& p) {
  const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  return v;
}
int32_t get32(const uint8_t*& p) {
  const uint32_t v = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
  p += 4;
  return static_cast<int32_t>(v);
}

constexpr std::size_t kBiasRec = 8;   // pi(2) + bias_ms(4) + samples(2)
constexpr std::size_t kBandRec = 10;  // khz(4) + successes(2) + snr_milli(4)

}  // namespace

std::size_t encodeStationBias(const StationBiasTable& t, void* buf, std::size_t cap) {
  const std::size_t n = t.count();
  const std::size_t need = 2 + n * kBiasRec;
  if (buf == nullptr || cap < need) return 0;

  uint8_t* p = static_cast<uint8_t*>(buf);
  *p++ = kVersion;
  *p++ = static_cast<uint8_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    const StationBias& b = t.at(i);
    put16(p, b.pi);
    // Milliseconds: the quantity is bounded by max_bias_us (2 s) and the extra
    // resolution would be noise — a station's bias is not knowable to the µs.
    put32(p, static_cast<int32_t>(b.bias_us / 1000));
    put16(p, static_cast<uint16_t>(b.samples > 0xFFFF ? 0xFFFF : b.samples));
  }
  return need;
}

bool decodeStationBias(const void* buf, std::size_t len, StationBiasTable* out) {
  if (buf == nullptr || out == nullptr || len < 2) return false;
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  if (*p++ != kVersion) return false;
  const std::size_t n = *p++;
  if (n > StationBiasTable::kMaxStations) return false;
  if (len < 2 + n * kBiasRec) return false;

  for (std::size_t i = 0; i < n; ++i) {
    const uint16_t pi = get16(p);
    const int32_t bias_ms = get32(p);
    const uint16_t samples = get16(p);
    // seed() re-applies max_bias_us, so a corrupt or out-of-policy figure is
    // dropped by the same rule that governs live measurements.
    out->seed(pi, static_cast<int64_t>(bias_ms) * 1000, samples);
  }
  return true;
}

std::size_t encodeBandStats(const Scheduler& s, void* buf, std::size_t cap) {
  const std::size_t n = s.bandCount();
  const std::size_t need = 2 + n * kBandRec;
  if (buf == nullptr || cap < need) return 0;

  uint8_t* p = static_cast<uint8_t*>(buf);
  *p++ = kVersion;
  *p++ = static_cast<uint8_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    const BandStats& b = s.bandStats(i);
    put32(p, b.khz);
    put16(p, static_cast<uint16_t>(b.successes > 0xFFFF ? 0xFFFF : b.successes));
    put32(p, static_cast<int32_t>(b.best_snr * 1000.0f));
  }
  return need;
}

bool decodeBandStats(const void* buf, std::size_t len, Scheduler* out) {
  if (buf == nullptr || out == nullptr || len < 2) return false;
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  if (*p++ != kVersion) return false;
  const std::size_t n = *p++;
  if (n > Scheduler::kMaxBands) return false;
  if (len < 2 + n * kBandRec) return false;

  for (std::size_t i = 0; i < n; ++i) {
    const int32_t khz = get32(p);
    const uint16_t successes = get16(p);
    const int32_t snr_milli = get32(p);
    // Matched BY FREQUENCY, not by index: the band list is a compile-time
    // decision and may well have been reordered or extended by the build that
    // reads this back. An index would then credit the wrong band.
    out->seedBandStats(khz, successes, static_cast<real>(snr_milli) / 1000.0f);
  }
  return true;
}

}  // namespace airtime
