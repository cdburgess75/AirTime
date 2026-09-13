#pragma once
//
// Digital-mode cycle phase — where we are inside an FT8/JS8/WSPR slot.
//
// This is what the whole device is FOR, finally said out loud on its own
// screen. Everything else here disciplines a clock; this turns that clock into
// something an operator reads mid-QSO: the bar fills across the transmit slot,
// the countdown says how long until the next one opens, and the millisecond
// digits are the accuracy we spent the project earning.
//
// It also makes the device a CHECK on the computer it is disciplining. If
// WSJT-X starts transmitting at a visibly different moment than the bar rolls
// over, the laptop's clock is wrong — caught by eye, from across the desk,
// without opening a log.
//
// ── Why plain modular arithmetic is correct here ────────────────────────────
// Every mode below has a period that divides 60 s evenly (or is a whole number
// of minutes), and the Unix epoch begins exactly on a minute boundary — and on
// an EVEN minute, which is what WSPR's 2-minute cycle needs. So slot boundaries
// are simply multiples of the period counted from the epoch, and no calendar,
// timezone or leap-second reasoning enters. FT4's 7.5 s is the only fractional
// one, which is why the arithmetic is in microseconds rather than seconds.
//
// Leap seconds: UTC gets one and every cycle shifts by a second with it, on
// every station on the band simultaneously. There is nothing to correct.

#include <cstddef>
#include <cstdint>

namespace airtime {

// A mode the operator would name, with the slot length they would quote.
struct CycleMode {
  const char* name;    // "FT8", "JS8 Fast" — as it appears on the panel
  int64_t period_us;   // slot length
};

// The modes worth putting on a knob, in the order a digital operator would
// meet them. FT8 first because it is why this device exists.
//
// FT4's 7.5 s and JS8's 6/10/30 s are the published slot lengths; WSPR and
// JT65 are here because a 2-minute or 1-minute cycle is exactly the case where
// a wall clock stops being good enough and you want a boundary you can see.
extern const CycleMode kCycleModes[];
extern const std::size_t kCycleModeCount;

// Where the given UTC instant falls inside a cycle of `period_us`.
struct CyclePhase {
  int64_t into_us = 0;     // elapsed since this slot opened
  int64_t remain_us = 0;   // until the next slot opens; never 0 except at t=0
  int64_t index = 0;       // absolute slot number since the epoch
  bool odd_slot = false;   // slot parity — FT8 alternates TX and RX on it
  float fraction = 0.0f;   // into_us / period_us, for drawing a bar
};

// Pure function of the instant: no state, no clock of its own, so the tests can
// stand at any moment in history and the panel can ask once per frame.
//
// utc_us before the epoch and non-positive periods are handled rather than
// trusted — this runs off a clock that is explicitly allowed to be wrong, and
// a display that divides by zero the moment a source misbehaves is not a
// display anyone should put on an instrument.
CyclePhase cyclePhaseAt(int64_t utc_us, int64_t period_us);

}  // namespace airtime
