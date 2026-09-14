#pragma once
//
// The TIME band: every frequency a standard time station keeps, one knob
// detent apart.
//
// Several stations share a frequency — WWV, WWVH and BPM all sit on 2500,
// 5000, 10000 and 15000 kHz, and HLA and YVTO join them on 5000 — so the band
// cannot offer "a station". It offers a frequency, and names everyone who may
// be on it. Which one comes out of the speaker is propagation's decision.
//
// Kept here, not in the firmware glue, so the stepping (between entries, off
// the ends, from a frequency that is not in the list) is host-tested.

#include <cstddef>
#include <cstdint>

namespace airtime {

struct TimeStationFreq {
  int32_t khz;
  const char* stations;   // who may be heard here, for the frequency-name line
};

// Ascending by frequency.
extern const TimeStationFreq kTimeStationFreqs[];
extern const std::size_t kTimeStationFreqCount;

// The stations on exactly this dial frequency, or nullptr.
const char* timeStationsAt(int32_t khz);

// The next list frequency above `khz` (dir > 0) or below it (dir < 0),
// wrapping at the ends. From a frequency between entries, the nearest entry in
// that direction. dir == 0 returns `khz` unchanged.
int32_t timeStationStep(int32_t khz, int dir);

}  // namespace airtime
