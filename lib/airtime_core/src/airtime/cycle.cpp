#include "cycle.h"

namespace airtime {

const CycleMode kCycleModes[] = {
    {"FT8",       15000000},
    {"FT4",        7500000},
    {"JS8 Normal",15000000},
    {"JS8 Fast",  10000000},
    {"JS8 Turbo",  6000000},
    {"JS8 Slow",  30000000},
    {"JT65/JT9",  60000000},
    {"WSPR",     120000000},
};
const std::size_t kCycleModeCount = sizeof(kCycleModes) / sizeof(kCycleModes[0]);

CyclePhase cyclePhaseAt(int64_t utc_us, int64_t period_us) {
  CyclePhase p;
  if (period_us <= 0) return p;   // an unusable period draws an empty bar

  // Floor division, not truncation. C++ rounds toward zero, which for a
  // pre-epoch instant would put the slot boundary on the WRONG SIDE of the
  // moment and hand back a negative into_us — a bar that fills backwards.
  // This clock starts life at zero and is allowed to be restored to something
  // absurd, so the case is reachable, and "reachable" is the whole standard.
  int64_t index = utc_us / period_us;
  int64_t into = utc_us % period_us;
  if (into < 0) {
    into += period_us;
    --index;
  }

  p.into_us = into;
  p.remain_us = period_us - into;
  p.index = index;
  // Parity of the slot itself, so it stays stable across a period change and
  // matches what WSJT-X calls even/odd. Modulo of a negative index is negative
  // in C++; compare against zero rather than testing a bit.
  p.odd_slot = (index % 2) != 0;
  p.fraction = (float)((double)into / (double)period_us);
  return p;
}

}  // namespace airtime
