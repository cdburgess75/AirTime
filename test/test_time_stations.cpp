#include "airtime/time_stations.h"
#include "test_framework.h"

#include <cstring>

using airtime::kTimeStationFreqCount;
using airtime::kTimeStationFreqs;
using airtime::timeStationStep;
using airtime::timeStationsAt;

AT_TEST(time_stations_list_is_ascending_and_unique) {
  for (std::size_t i = 1; i < kTimeStationFreqCount; ++i) {
    AT_CHECK(kTimeStationFreqs[i - 1].khz < kTimeStationFreqs[i].khz);
  }
}

// The table the owner asked for, frequency by frequency.
AT_TEST(time_stations_names_every_station_on_its_frequencies) {
  const auto has = [](int32_t khz, const char* station) {
    const char* s = timeStationsAt(khz);
    return s != nullptr && std::strstr(s, station) != nullptr;
  };
  for (int32_t f : {2500, 5000, 10000, 15000, 20000}) AT_CHECK(has(f, "WWV"));
  for (int32_t f : {2500, 5000, 10000, 15000}) AT_CHECK(has(f, "WWVH"));
  for (int32_t f : {2500, 5000, 10000, 15000}) AT_CHECK(has(f, "BPM"));
  for (int32_t f : {3330, 7850, 14670}) AT_CHECK(has(f, "CHU"));
  for (int32_t f : {4996, 9996, 14996}) AT_CHECK(has(f, "RWM"));
  AT_CHECK(has(5000, "HLA"));
  AT_CHECK(has(5000, "YVTO"));
  AT_CHECK(!has(20000, "WWVH"));
  AT_CHECK(timeStationsAt(6000) == nullptr);
}

AT_TEST(time_stations_step_moves_one_entry_and_wraps) {
  AT_CHECK(timeStationStep(10000, +1) == 14670);
  AT_CHECK(timeStationStep(10000, -1) == 9996);
  AT_CHECK(timeStationStep(4996, +1) == 5000);   // RWM and WWV 4 kHz apart stay distinct
  AT_CHECK(timeStationStep(20000, +1) == 2500);  // off the top
  AT_CHECK(timeStationStep(2500, -1) == 20000);  // off the bottom
  AT_CHECK(timeStationStep(10000, 0) == 10000);
}

// A frequency not in the list — the band opened somewhere else, or was
// digit-tuned — goes to the nearest entry in the direction turned.
AT_TEST(time_stations_step_from_between_entries) {
  AT_CHECK(timeStationStep(6000, +1) == 7850);
  AT_CHECK(timeStationStep(6000, -1) == 5000);
  AT_CHECK(timeStationStep(1000, +1) == 2500);
  AT_CHECK(timeStationStep(1000, -1) == 20000);
  AT_CHECK(timeStationStep(25000, -1) == 20000);
}
