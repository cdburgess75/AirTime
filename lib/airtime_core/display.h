#pragma once
//
// Display state and formatting — PLAN.md §5.
//
//     14:22:07 UTC
//     ±60 ms · RDS+WWV · sync 3h ago · NTP: 2 clients
//
// UTC is primary (FT8 wants UTC). The status line exists to make the device
// honest: uncertainty is always on screen, and an unsynced device says so
// instead of showing a confident-looking wrong time.
//
// Formatting lives here, in pure logic, so the exact strings are unit-tested;
// the Ui adapter only pushes them to the TFT.

#include <cstddef>
#include <cstdint>

#include "scheduler.h"
#include "types.h"

namespace airtime {

// Which sources have contributed recently (a bitmask, so "RDS+WWV" can be shown).
constexpr uint8_t kSrcRds = 1 << 0;
constexpr uint8_t kSrcWwv = 1 << 1;
constexpr uint8_t kSrcManual = 1 << 2;

uint8_t sourceBit(Source s);

struct DisplayState {
  bool clock_valid = false;   // false only before any seed at all
  bool synced = false;        // uncertainty within the sync threshold
  bool ever_synced = false;   // distinguishes cold boot from stale sync
  int64_t utc_us = 0;
  int64_t uncertainty_us = 0;
  int64_t since_sync_us = 0;
  uint8_t sources = 0;        // bitmask of recent contributors
  int ntp_clients = 0;
  Phase phase = Phase::Acquiring;
};

// "14:22:07 UTC" — or "--:--:-- UTC" when there is no time at all.
void formatUtcLine(const DisplayState& st, char* buf, std::size_t n);

// "±60 ms · RDS+WWV · sync 3h ago · NTP: 2 clients"
// or "UNSYNCED — last-known + drift · ..." when the estimate has gone stale.
void formatStatusLine(const DisplayState& st, char* buf, std::size_t n);

// Pieces, exposed for reuse and testing.
void formatUncertainty(int64_t us, char* buf, std::size_t n);  // "±60 ms", "±1.2 s"
void formatAge(int64_t us, char* buf, std::size_t n);          // "45s", "12m", "3h"
void formatSources(uint8_t mask, char* buf, std::size_t n);    // "RDS+WWV", "none"

}  // namespace airtime
