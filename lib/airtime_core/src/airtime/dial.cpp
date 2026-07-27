#include "dial.h"

namespace airtime {

int pickDialBand(const DialBandSpan* bands, std::size_t count,
                 int32_t khz, bool want_fm, uint8_t want_mode) {
  if (bands == nullptr) return -1;

  int best = -1;
  int32_t best_span = 0;

  for (std::size_t i = 0; i < count; ++i) {
    const DialBandSpan& b = bands[i];
    if (b.fm != want_fm) continue;
    if (khz < b.min_khz || khz > b.max_khz) continue;

    const int32_t span = b.max_khz - b.min_khz;
    const bool better = (best < 0) || (span < best_span) ||
                        (span == best_span && b.mode == want_mode);
    if (better) {
      best = static_cast<int>(i);
      best_span = span;
    }
  }
  return best;
}

}  // namespace airtime
