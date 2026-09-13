#pragma once
//
// Scheduled net directory — "what is on the air right now".
//
// This is the feature the clock earns. A receiver with a vague idea of the time
// can only offer a list of frequencies; one that knows UTC to milliseconds can
// answer the question an operator actually has, which is not "what frequency is
// the Maritime Mobile Net on" but "is it on NOW, and if not, how long until it
// is". Everything here keys off the disciplined clock.
//
// Times are UTC minutes past midnight and days are a UTC weekday mask, because
// that is how net schedules are published and because it sidesteps the entire
// local-time and DST question — a net at 1200Z is at 1200Z in March and in
// July. The display can still show the operator's local time beside it
// (timezone.h); the SCHEDULE stays in the units it was written in.

#include <cstddef>
#include <cstdint>

namespace airtime {

// Weekday mask. Bit 0 is Sunday, matching the usual publication order.
// Interpreted in the net's own anchor zone — see NetAnchor.
enum : uint8_t {
  kSun = 1 << 0, kMon = 1 << 1, kTue = 1 << 2, kWed = 1 << 3,
  kThu = 1 << 4, kFri = 1 << 5, kSat = 1 << 6,
  kDaily = 0x7F,
  kWeekdays = kMon | kTue | kWed | kThu | kFri,

  // A net that exists but keeps no schedule — the Hurricane Watch Net runs
  // when a storm activates it and not otherwise. Both queries below skip
  // these, so such a net is never "on the air now" and never "next". Listing
  // one as active 24/7 so it stays visible in a UI is a lie this device in
  // particular has no business telling.
  kOnDemand = 0,
};

// Mode as the operator would set it, not as the chip encodes it.
enum class NetMode : uint8_t { Lsb, Usb, Cw, Am, Fm };

// What `start_min` and `days` are measured against.
//
// Most published net schedules are not UTC schedules wearing a UTC hat. A net
// that meets at 6:30 PM Central meets at 6:30 PM Central in January too — the
// UTC time is what moves, by an hour, twice a year. Tables that quote UTC have
// almost always frozen one half of the year: the SkyWave directory this
// firmware's list came from says so outright, that its UTC columns are anchored
// to US daylight time and shift +1 h in winter.
//
// Copying those numbers would have made the net display silently an hour wrong
// from November to March. So a net records the frame its schedule was WRITTEN
// in, and the conversion happens here, where the DST rule already lives.
enum class NetAnchor : uint8_t { Utc, UsEastern, UsCentral };

struct HamNet {
  const char* name;
  int32_t khz;            // dial frequency
  NetMode mode;
  uint8_t days;           // weekday mask in the anchor zone, or kOnDemand
  int16_t start_min;      // minutes past local midnight in the anchor zone
  int16_t duration_min;   // may run past midnight; wrapping is handled
  NetAnchor anchor = NetAnchor::Utc;
};

// Index of a net on the air at this instant, or -1. When schedules overlap the
// earliest-listed wins, so order the table by preference.
int netActiveAt(const HamNet* nets, std::size_t count, int64_t utc_s);

// Index of the next net due to start, or -1 if the table is empty.
// *minutes_until receives how long until it does (0 if one is on the air now).
// Looks a full week ahead, so a weekly net is still found on the wrong day.
int netNextAt(const HamNet* nets, std::size_t count, int64_t utc_s,
              int* minutes_until);

// UTC weekday at this instant, 0 = Sunday.
int utcWeekday(int64_t utc_s);

}  // namespace airtime
