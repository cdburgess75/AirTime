#pragma once
//
// Local time for the operator's benefit — PLAN.md §5's display, nothing more.
//
// AirTime keeps and serves UTC; that is what NTP carries and what FT8 runs on.
// This exists only so the screen can also show the time the person holding the
// radio actually thinks in.
//
// ── Why this is not just an offset ──────────────────────────────────────────
//
// A UTC offset cannot name a zone, and a fixed name cannot survive the year.
// UTC-5 is CDT in July and EST in January; UTC-6 is CST in January and MDT in
// July. Labelling a clock "CST" year-round is wrong from March to November,
// and setting the offset by hand twice a year is exactly the kind of chore a
// self-setting clock exists to abolish. So a zone here is a rule, not a
// number: a standard offset, the two names, and whether it observes DST.
//
// The DST rule implemented is the US federal one (second Sunday in March to
// first Sunday in November). Zones outside that rule are supported as
// fixed-offset entries, which is honest — a wrong DST rule would be worse than
// none — and the table is trivial to extend if this ever needs Europe.

#include <cstdint>

namespace airtime {

struct TimeZoneRule {
  const char* std_name;   // "CST"
  const char* dst_name;   // "CDT", or nullptr for a zone with no DST
  int32_t std_offset_s;   // standard-time offset from UTC, seconds
};

// Is US daylight time in force at this instant, for a zone whose standard
// offset is std_offset_s? Transitions are defined in local time (02:00 local
// standard in spring, 02:00 local daylight in autumn) and converted to UTC
// here, so the answer is correct within the hour of the switch, not just to
// the day.
bool usDstInEffect(int64_t utc_s, int32_t std_offset_s);

// Local time for a zone, plus the name that applies at that instant.
// `name_out` may be null. Returns seconds since the Unix epoch, shifted.
int64_t localEpochS(const TimeZoneRule& tz, int64_t utc_s, const char** name_out);

// Zones the shipped firmware offers. Extend freely — it is a plain table.
extern const TimeZoneRule kZoneEastern;   // EST/EDT
extern const TimeZoneRule kZoneCentral;   // CST/CDT
extern const TimeZoneRule kZoneMountain;  // MST/MDT
extern const TimeZoneRule kZoneArizona;   // MST, no DST
extern const TimeZoneRule kZonePacific;   // PST/PDT
extern const TimeZoneRule kZoneAlaska;    // AKST/AKDT
extern const TimeZoneRule kZoneHawaii;    // HST, no DST
extern const TimeZoneRule kZoneUtc;       // UTC

}  // namespace airtime
