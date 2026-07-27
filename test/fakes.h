#pragma once
//
// Host fakes for the hardware seam — a simulated ATS Mini.
//
// Together with Sim (bottom of this file) these implement enough of the physical
// world to run the real AirTimeApp end to end on a laptop: a crystal that drifts
// at a configurable rate, FM stations that transmit RDS clock-time (correctly or
// otherwise), WWV minute markers that only arrive on bands that are propagating,
// a receive chain with latency, WiFi that can be up or down, and NVS.
//
// These are test-only and may use the STL freely; nothing in lib/airtime_core
// includes this file.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "airtime/app.h"
#include "airtime/hal.h"
#include "airtime/rds_ct.h"

namespace airtime_fake {

using namespace airtime;

// --- Clock ------------------------------------------------------------------
// Monotonic time as the device sees it. Drifts relative to true time by
// crystal_ppm (positive = the device runs fast).
class FakeClock : public IMonotonicClock {
 public:
  int64_t mono_us = 0;
  int64_t nowUs() const override { return mono_us; }
};

// --- FM / RDS ---------------------------------------------------------------
struct FakeStation {
  int32_t khz = 0;
  uint16_t pi = 0;
  bool sends_ct = true;    // many US stations send no CT group at all (§4)
  // Some stations send wrong/offset CT (§4). Modelled as a TIMING offset: the
  // group asserts minute M but arrives error_us away from it. That is both the
  // physical error mode and the only one that survives — RDS CT carries hours
  // and minutes only, so encoding an error into the content silently discards
  // anything under 60 s.
  int64_t error_us = 0;
};

// Encode a UTC instant as an RDS group 4A (the inverse of decodeRdsClockTime).
inline void buildCtGroup(int64_t utc_epoch_s, uint16_t pi, RdsGroup* g) {
  const int64_t days = utc_epoch_s / 86400;
  const int64_t sod = utc_epoch_s - days * 86400;
  const int32_t mjd = static_cast<int32_t>(days + 40587);
  const int hour = static_cast<int>(sod / 3600);
  const int minute = static_cast<int>((sod % 3600) / 60);

  g->a = pi;
  g->b = static_cast<uint16_t>((4u << 12) | (0u << 11) |
                               (static_cast<uint32_t>(mjd >> 15) & 0x3u));
  g->c = static_cast<uint16_t>(((static_cast<uint32_t>(mjd) & 0x7FFFu) << 1) |
                               (static_cast<uint32_t>(hour >> 4) & 0x1u));
  g->d = static_cast<uint16_t>(((static_cast<uint32_t>(hour) & 0x0Fu) << 12) |
                               ((static_cast<uint32_t>(minute) & 0x3Fu) << 6));
}

class FakeRdsSource : public IRdsSource {
 public:
  std::vector<FakeStation> stations;
  int tune_count = 0;  // retunes are observable: the real radio has ONE tuner

  void tuneKhz(int32_t khz) override {
    tuned_ = khz;
    ++tune_count;
  }
  int32_t tunedKhz() const override { return tuned_; }

  bool poll(RdsGroup* out) override {
    if (queue_.empty()) return false;
    *out = queue_.front();
    queue_.pop_front();
    return true;
  }

  // Emit a CT group from the tuned station at each true minute boundary.
  void pump(int64_t true_utc_us, int64_t mono_us, int64_t prev_true_utc_us) {
    constexpr int64_t kMin = 60000000;
    const int64_t first = (prev_true_utc_us / kMin + 1) * kMin;
    for (int64_t m = first; m <= true_utc_us; m += kMin) {
      for (const FakeStation& s : stations) {
        if (s.khz != tuned_ || !s.sends_ct) continue;
        RdsGroup g;
        buildCtGroup(m / 1000000, s.pi, &g);   // asserts the exact minute
        // ...but arrives error_us away from it, which is what makes the station
        // wrong. Offset seen by the voter is therefore -error_us.
        g.mono_us = mono_us - (true_utc_us - m) + s.error_us;
        queue_.push_back(g);
      }
    }
  }

  std::size_t pending() const { return queue_.size(); }

 private:
  int32_t tuned_ = 0;
  std::deque<RdsGroup> queue_;
};

// --- HF / WWV ---------------------------------------------------------------
// Emits Goertzel power estimates. A 20 ms block is a deliberate choice: it still
// resolves 1000 Hz comfortably while keeping leading-edge quantization small
// enough to support the sub-100 ms phase alignment WWV is there to provide.
class FakeWwvSampler : public IWwvSampler {
 public:
  int64_t block_us = 20000;
  int64_t chain_delay_us = 0;   // SI4732 DSP + amp + ADC latency
  int64_t marker_us = 800000;   // WWV minute marker length
  // Defaults are the levels MEASURED on the owner's ATS Mini (Milestone 0 §6),
  // normalised by half ADC scale — not round numbers. Simulating at realistic
  // levels is what catches threshold bugs like the min_power default that was
  // 5x above real signal.
  real tone_power = 1.9e-3f;
  real noise_power = 7.7e-5f;
  std::vector<int32_t> propagating_bands;  // empty => nothing is heard

  void tuneKhz(int32_t khz) override { tuned_ = khz; }
  void start() override { running_ = true; }
  void stop() override { running_ = false; }
  bool isRunning() const override { return running_; }

  bool nextPower(int64_t* mono_us, real* power) override {
    if (queue_.empty()) return false;
    *mono_us = queue_.front().first;
    *power = queue_.front().second;
    queue_.pop_front();
    return true;
  }

  void pump(int64_t true_utc_us, int64_t mono_us) {
    if (!running_) {
      next_block_true_ = true_utc_us;  // resync on restart
      return;
    }
    if (next_block_true_ == 0) next_block_true_ = true_utc_us;

    while (next_block_true_ <= true_utc_us) {
      const int64_t t = next_block_true_;
      // Map this true instant back to device monotonic time.
      const int64_t stamp = mono_us - (true_utc_us - t);
      queue_.push_back({stamp, powerAt(t)});
      next_block_true_ += block_us;
    }
  }

  int32_t tunedKhz() const { return tuned_; }

 private:
  bool propagates() const {
    for (int32_t b : propagating_bands) {
      if (b == tuned_) return true;
    }
    return false;
  }

  real powerAt(int64_t true_utc_us) const {
    if (!propagates()) return noise_power;
    // The marker is heard chain_delay_us after it is transmitted.
    int64_t rel = (true_utc_us - chain_delay_us) % 60000000;
    if (rel < 0) rel += 60000000;
    return rel < marker_us ? tone_power : noise_power;
  }

  bool running_ = false;
  int32_t tuned_ = 0;
  int64_t next_block_true_ = 0;
  std::deque<std::pair<int64_t, real>> queue_;
};

// --- WiFi -------------------------------------------------------------------
class FakeWiFi : public IWiFiControl {
 public:
  void bringUp() override {
    up_ = true;
    ++up_count;
  }
  void tearDown() override { up_ = false; }
  bool isUp() const override { return up_; }
  int up_count = 0;

 private:
  bool up_ = false;
};

// --- NVS --------------------------------------------------------------------
class FakeStore : public ITimeStore {
 public:
  bool has_drift = false, has_utc = false;
  double drift_ppm = 0.0;
  int64_t last_utc_us = 0;
  int drift_saves = 0, utc_saves = 0;

  bool loadDriftPpm(double* ppm) override {
    if (!has_drift) return false;
    *ppm = drift_ppm;
    return true;
  }
  void saveDriftPpm(double ppm) override {
    drift_ppm = ppm;
    has_drift = true;
    ++drift_saves;
  }
  bool loadLastUtc(int64_t* utc) override {
    if (!has_utc) return false;
    *utc = last_utc_us;
    return true;
  }
  void saveLastUtc(int64_t utc) override {
    last_utc_us = utc;
    has_utc = true;
    ++utc_saves;
  }

  // Blobs, kept as plain bytes so a test can power-cycle by handing the same
  // FakeStore to a fresh AirTimeApp — exactly what NVS does for the device.
  std::map<std::string, std::vector<uint8_t>> blobs;
  int blob_saves = 0;

  bool loadBlob(const char* key, void* buf, std::size_t cap,
                std::size_t* out_len) override {
    auto it = blobs.find(key);
    if (it == blobs.end() || it->second.size() > cap) return false;
    std::memcpy(buf, it->second.data(), it->second.size());
    if (out_len != nullptr) *out_len = it->second.size();
    return true;
  }

  void saveBlob(const char* key, const void* buf, std::size_t len) override {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    blobs[key].assign(p, p + len);
    ++blob_saves;
  }
};

// --- The simulated world ----------------------------------------------------
class Sim {
 public:
  FakeClock clock;
  FakeRdsSource rds;
  FakeWwvSampler wwv;
  FakeWiFi wifi;
  FakeStore store;

  int64_t true_utc_us = 0;
  double crystal_ppm = 0.0;  // positive => device clock runs fast

  // Set by advance(): true if WiFi and the ADC were ever live simultaneously.
  bool adc_wifi_conflict = false;

  airtime::AppDeps deps() {
    airtime::AppDeps d;
    d.clock = &clock;
    d.rds = &rds;
    d.wwv = &wwv;
    d.wifi = &wifi;
    d.store = &store;
    return d;
  }

  // Advance true time by `delta_us`, stepping the world and running the device's
  // main loop as it goes.
  void advance(int64_t delta_us, airtime::AirTimeApp* app, int64_t step_us = 10000) {
    int64_t remaining = delta_us;
    while (remaining > 0) {
      const int64_t step = remaining < step_us ? remaining : step_us;
      const int64_t prev_true = true_utc_us;

      true_utc_us += step;
      elapsed_us_ += step;
      // Derive monotonic time from TOTAL elapsed, never by accumulating a
      // per-step delta: at 10 ms steps a 28 ppm error is 0.28 µs per step, which
      // would round to zero every time and silently simulate a perfect crystal.
      clock.mono_us = static_cast<int64_t>(std::llround(
          static_cast<double>(elapsed_us_) * (1.0 + crystal_ppm / 1e6)));

      rds.pump(true_utc_us, clock.mono_us, prev_true);
      wwv.pump(true_utc_us, clock.mono_us);

      if (app != nullptr) app->loop();

      // The invariant that protects ADC2 (PLAN.md §2).
      if (wifi.isUp() && wwv.isRunning()) adc_wifi_conflict = true;

      remaining -= step;
    }
  }

  // Device clock error against truth, in µs (positive => device is fast).
  int64_t clockErrorUs(const airtime::AirTimeApp& app) const {
    return app.arbiter().utcAt(clock.mono_us) - true_utc_us;
  }

 private:
  int64_t elapsed_us_ = 0;
};

}  // namespace airtime_fake
