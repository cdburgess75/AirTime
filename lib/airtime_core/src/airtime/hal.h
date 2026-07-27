#pragma once
//
// The hardware seam — abstract interfaces the firmware implements and the host
// tests fake. See docs/ARCHITECTURE.md.
//
// Everything above this line is pure logic; everything below it is a thin shim
// over a peripheral. Keeping the seam narrow is what lets the entire device be
// simulated on a laptop (see test/fakes.h) and what keeps the risky, hard-to-
// debug part of the project small.
//
// Design notes:
//  * Interfaces are polled and non-blocking. The app drives everything from one
//    loop; no callbacks, no threads in the core.
//  * IWwvSampler hands out *power estimates*, not raw samples. On device the ADC
//    read plus the Goertzel run together in a tight loop pinned to core 2 (the
//    Goertzel implementation is ours — lib/airtime_core/goertzel.h); shipping raw
//    audio across to core 0 would waste RAM and add jitter to the one measurement
//    whose timing actually matters.
//  * Timestamps are monotonic µs and are produced as close to the physical event
//    as possible (the leading edge of a tone, the arrival of an RDS group).

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

// --- Time -------------------------------------------------------------------
// Monotonic microseconds. On device: esp_timer_get_time().
class IMonotonicClock {
 public:
  virtual ~IMonotonicClock() = default;
  virtual int64_t nowUs() const = 0;
};

// --- FM / RDS ---------------------------------------------------------------
// One RDS group as the SI4732 hands it over: four 16-bit information words with
// checkwords already stripped. Block A is the station's PI code.
struct RdsGroup {
  uint16_t a = 0;
  uint16_t b = 0;
  uint16_t c = 0;
  uint16_t d = 0;
  int64_t mono_us = 0;  // when the group arrived
};

class IRdsSource {
 public:
  virtual ~IRdsSource() = default;
  virtual void tuneKhz(int32_t khz) = 0;
  virtual int32_t tunedKhz() const = 0;
  // Non-blocking: returns false when no group is waiting.
  virtual bool poll(RdsGroup* out) = 0;
};

// --- HF / WWV ---------------------------------------------------------------
// MUST only be sampled while WiFi is down (ADC2 constraint, PLAN.md §2). The
// Scheduler guarantees the ordering; the adapter may assert it.
class IWwvSampler {
 public:
  virtual ~IWwvSampler() = default;
  virtual void tuneKhz(int32_t khz) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual bool isRunning() const = 0;
  // Next Goertzel power estimate and the monotonic time of its block.
  // Non-blocking: returns false when nothing is ready.
  virtual bool nextPower(int64_t* mono_us, real* power) = 0;
};

// --- Radio link -------------------------------------------------------------
class IWiFiControl {
 public:
  virtual ~IWiFiControl() = default;
  virtual void bringUp() = 0;    // SoftAP + NTP socket
  virtual void tearDown() = 0;
  virtual bool isUp() const = 0;
};

// --- Persistence (NVS) ------------------------------------------------------
class ITimeStore {
 public:
  virtual ~ITimeStore() = default;
  virtual bool loadDriftPpm(double* ppm) = 0;
  virtual void saveDriftPpm(double ppm) = 0;
  virtual bool loadLastUtc(int64_t* utc_us) = 0;
  virtual void saveLastUtc(int64_t utc_us) = 0;

  // Named opaque blobs, for the things the device LEARNS: which stations run
  // how late, which HF band actually propagates here. These arrive as one
  // generic pair rather than a virtual per fact, so adding the next learned
  // thing does not force every adapter and fake to grow a method.
  //
  // Default to "no storage" so a partial adapter still compiles and simply
  // relearns from scratch — the same graceful degradation the app already
  // gives a null store.
  //
  // loadBlob returns false when the key is absent; *out_len is the number of
  // bytes written into buf.
  virtual bool loadBlob(const char* key, void* buf, std::size_t cap,
                        std::size_t* out_len) {
    (void)key; (void)buf; (void)cap; (void)out_len;
    return false;
  }
  virtual void saveBlob(const char* key, const void* buf, std::size_t len) {
    (void)key; (void)buf; (void)len;
  }
};

}  // namespace airtime
