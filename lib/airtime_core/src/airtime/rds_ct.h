#pragma once
//
// RDS group 4A clock-time (CT) decode.
//
// PLAN.md §4 Tier 1 / Milestone 1: FM RDS carries a clock-time group that gives
// full date + UTC time. The SI4732 decodes RDS in hardware and hands us the four
// 16-bit information words (blocks A-D, checkwords already stripped). This module
// turns a 4A group into a UTC timestamp; timing alignment against the reception
// instant and cross-station voting are handled by callers (see station_vote.h).
//
// Bit layout of a 4A group (per the RDS/RBDS standard):
//   Block B: [15:12] group type (=0100), [11] version (=0 for A), [10] TP,
//            [9:5] PTY, [4:2] unused, [1:0] MJD[16:15]
//   Block C: [15:1] MJD[14:0],  [0] hour[4]
//   Block D: [15:12] hour[3:0], [11:6] minute[5:0],
//            [5] local-offset sense (0=+,1=-), [4:0] local offset in half-hours

#include <cstdint>

namespace airtime {

struct RdsClockTime {
  int year;               // full year, e.g. 2026
  int month;              // 1..12
  int day;                // 1..31
  int hour;               // 0..23  (UTC)
  int minute;             // 0..59  (UTC)
  int offset_half_hours;  // signed local offset from UTC, in 30-min units
  int64_t utc_epoch_s;    // seconds since 1970-01-01T00:00:00Z (minute resolution)
};

// Decode a group 4A. Returns false (and leaves *out untouched) if the group is
// not a valid 4A or any field is out of range.
bool decodeRdsClockTime(uint16_t blockA, uint16_t blockB, uint16_t blockC,
                        uint16_t blockD, RdsClockTime* out);

// Modified Julian Day <-> civil date (exposed for reuse and testing).
void mjdToCivil(int32_t mjd, int* year, int* month, int* day);
int32_t civilToMjd(int year, int month, int day);

// RDS block-B helpers.
inline int rdsGroupType(uint16_t blockB) { return (blockB >> 12) & 0x0F; }
inline bool rdsIsVersionB(uint16_t blockB) { return ((blockB >> 11) & 0x1) != 0; }

}  // namespace airtime
