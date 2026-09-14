#include "airtime/time_stations.h"

namespace airtime {

const TimeStationFreq kTimeStationFreqs[] = {
    {2500, "WWV WWVH BPM"},
    {3330, "CHU"},
    {4996, "RWM (CW)"},             // Morse only: silent in AM, use USB
    {5000, "WWV WWVH BPM HLA YVTO"},
    {7850, "CHU"},
    {9996, "RWM (CW)"},
    {10000, "WWV WWVH BPM"},
    {14670, "CHU"},
    {14996, "RWM (CW)"},
    {15000, "WWV WWVH BPM"},
    {20000, "WWV"},
};

const std::size_t kTimeStationFreqCount =
    sizeof(kTimeStationFreqs) / sizeof(kTimeStationFreqs[0]);

const char* timeStationsAt(int32_t khz) {
  for (std::size_t i = 0; i < kTimeStationFreqCount; ++i) {
    if (kTimeStationFreqs[i].khz == khz) return kTimeStationFreqs[i].stations;
  }
  return nullptr;
}

int32_t timeStationStep(int32_t khz, int dir) {
  if (dir == 0 || kTimeStationFreqCount == 0) return khz;
  const std::size_t last = kTimeStationFreqCount - 1;
  if (dir > 0) {
    for (std::size_t i = 0; i < kTimeStationFreqCount; ++i) {
      if (kTimeStationFreqs[i].khz > khz) return kTimeStationFreqs[i].khz;
    }
    return kTimeStationFreqs[0].khz;
  }
  for (std::size_t i = kTimeStationFreqCount; i-- > 0;) {
    if (kTimeStationFreqs[i].khz < khz) return kTimeStationFreqs[i].khz;
  }
  return kTimeStationFreqs[last].khz;
}

}  // namespace airtime
