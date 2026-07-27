#include "station_bias.h"

#include <cmath>

namespace airtime {

static inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

StationBiasTable::StationBiasTable(const StationBiasConfig& cfg) : cfg_(cfg) {}

const StationBias* StationBiasTable::find(uint16_t pi) const {
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].pi == pi) return &rows_[i];
  }
  return nullptr;
}

StationBias* StationBiasTable::row(uint16_t pi) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].pi == pi) return &rows_[i];
  }
  if (count_ >= kMaxStations) {
    // Full. Evict the least-measured station: entries with few samples are the
    // ones still not trusted anyway, so nothing that is actually steering the
    // clock is ever thrown out to make room for a passing signal.
    std::size_t worst = 0;
    for (std::size_t i = 1; i < count_; ++i) {
      if (rows_[i].samples < rows_[worst].samples) worst = i;
    }
    rows_[worst] = StationBias{};
    rows_[worst].pi = pi;
    return &rows_[worst];
  }
  rows_[count_] = StationBias{};
  rows_[count_].pi = pi;
  return &rows_[count_++];
}

void StationBiasTable::observe(uint16_t pi, int64_t lateness_us, int64_t mono_us) {
  // Broken, not biased — let the voter discard it rather than teaching the
  // table to "correct" a station that is minutes out into false agreement.
  if (iabs64(lateness_us) > cfg_.max_bias_us) return;

  StationBias* r = row(pi);
  r->last_obs_mono = mono_us;
  if (r->samples == 0) {
    r->bias_us = lateness_us;  // no prior; the first measurement IS the estimate
  } else {
    const double prev = static_cast<double>(r->bias_us);
    r->bias_us = static_cast<int64_t>(
        llround(prev + cfg_.alpha * (static_cast<double>(lateness_us) - prev)));
  }
  ++r->samples;
}

int64_t StationBiasTable::correction(uint16_t pi) const {
  const StationBias* r = find(pi);
  if (r == nullptr || r->samples < cfg_.min_samples) return 0;
  return r->bias_us;
}

void StationBiasTable::seed(uint16_t pi, int64_t bias_us, int samples) {
  if (iabs64(bias_us) > cfg_.max_bias_us) return;
  StationBias* r = row(pi);
  r->bias_us = bias_us;
  r->samples = samples;
}

}  // namespace airtime
