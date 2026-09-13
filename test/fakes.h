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
#include "airtime/wwv_timecode.h"

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

// --- The tuner --------------------------------------------------------------
// The SI4732 has ONE tuner, and this struct is it.
//
// The fakes below used to be two independent radios — each kept its own
// `tuned_` and consulted nobody. That lie hid the two bugs that reached
// hardware anyway: a "Radio" screen reading 7200 while the chip played an FM
// station, and a listen window that opened on 40 m while every log line said
// 15000, because the app's cached band matched the band it wanted and so it
// never retuned. Neither can be expressed when each fake owns its own dial.
//
// So the truth lives here, once. The adapters' `tunedKhz()` values remain —
// deliberately — as what they are on the device: CACHED CLAIMS about a chip
// that may have been tuned by somebody else since. A test can now assert on
// either, and on the difference, which IS the bug class.
//
// operatorTune() models the stock firmware touching the chip directly
// (selectBand goes through neither adapter), which is what an operator turning
// the dial in radio mode does to AirTime's beliefs.
struct FakeTuner {
  enum class Band : uint8_t { None, Fm, Am };
  Band band = Band::None;
  int32_t khz = 0;

  void tuneFm(int32_t k) { band = Band::Fm; khz = k; }
  void tuneAm(int32_t k) { band = Band::Am; khz = k; }
  bool onFm(int32_t k) const { return band == Band::Fm && khz == k; }
  bool onAm(int32_t k) const { return band == Band::Am && khz == k; }
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
  int rssi = 40;           // what a survey scan would read here
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
  FakeTuner* tuner = nullptr;   // wired by Sim; the one dial
  int tune_count = 0;  // retunes are observable: the real radio has ONE tuner

  void tuneKhz(int32_t khz) override {
    tuned_ = khz;
    ++tune_count;
    if (tuner != nullptr) tuner->tuneFm(khz);
  }
  // The adapter's cached claim — NOT necessarily where the chip is. See
  // FakeTuner: keeping the two distinct is the entire point.
  int32_t tunedKhz() const override { return tuned_; }

  // Signal strength of whatever is on the tuned frequency. The survey uses
  // this to skip empty channels in 200 ms instead of dwelling 80 s on noise.
  // Off FM entirely, the real adapter reports -1 (atRdsRssi refuses to read a
  // shortwave RSSI into an FM survey).
  int signalStrength() const override {
    if (tuner != nullptr && tuner->band != FakeTuner::Band::Fm) return -1;
    for (const FakeStation& s : stations) {
      if (s.khz == rxKhz()) return s.rssi;
    }
    return 2;   // band noise
  }

  bool poll(RdsGroup* out) override {
    if (queue_.empty()) return false;
    *out = queue_.front();
    queue_.pop_front();
    return true;
  }

  // Emit a CT group from the RECEIVED station at each true minute boundary —
  // received per the shared tuner, not per this adapter's cache. On AM, or on
  // an FM frequency nobody transmits on, nothing arrives; the SI4735 library
  // does not even perform the I2C read outside FM mode.
  void pump(int64_t true_utc_us, int64_t mono_us, int64_t prev_true_utc_us) {
    constexpr int64_t kMin = 60000000;
    const int32_t rx = rxKhz();
    if (rx == 0) return;
    const int64_t first = (prev_true_utc_us / kMin + 1) * kMin;
    for (int64_t m = first; m <= true_utc_us; m += kMin) {
      for (const FakeStation& s : stations) {
        if (s.khz != rx || !s.sends_ct) continue;
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
  // Where RDS is actually being received from: the chip's dial if a tuner is
  // wired (0 when the chip is not on FM at all), this fake's own cache when a
  // bare test runs it standalone.
  int32_t rxKhz() const {
    if (tuner == nullptr) return tuned_;
    return tuner->band == FakeTuner::Band::Fm ? tuner->khz : 0;
  }

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

  FakeTuner* tuner = nullptr;   // wired by Sim; the one dial
  int tune_count = 0;  // as with FakeRdsSource: retunes are observable

  // What the detector is pointed at — assertable, so a test can prove the mode
  // machinery repointed it (700 Hz / 5 ms for CW, back to 1000 Hz / 20 ms for
  // WWV) rather than trusting the sequencing.
  real detector_tone_hz = 1000.0f;
  int64_t detector_block_us = 20000;
  int rejected_detector_sets = 0;   // calls made while running — the race

  // ── The 100 Hz subcarrier channel ──────────────────────────────────────────
  // A propagating band carries the real WWV time code: one pulse per second,
  // starting sub_lead_us after the second, whose width is the symbol for that
  // second of the minute (encodeFrame — the shared-table inverse, so this
  // proves the CHAIN, not the bit map). Power levels follow the marker's
  // measured-scale convention: the subcarrier gets about a quarter of the tick
  // tones' modulation, and the 20 Hz bin's floor sits under the marker bin's.
  int64_t sub_block_us = 50000;
  real sub_tone_power = 4.7e-4f;
  real sub_noise_power = 3.0e-5f;
  int64_t sub_lead_us = 30000;      // NIST: the code pulse opens 30 ms in
  real sub_detector_hz = 100.0f;    // what the app pointed the channel at
  int64_t sub_detector_block_us = 50000;
  bool sub_carrier_present = true;  // false: band propagates, code absent

  void tuneKhz(int32_t khz) override {
    tuned_ = khz;
    ++tune_count;
    if (tuner != nullptr) tuner->tuneAm(khz);
  }
  bool setDetector(real tone_hz, int64_t bus) override {
    // Same contract as Esp32WwvSampler: never mid-run. A caller that tries is
    // exactly the cross-core cfg_ race the real adapter refuses.
    if (running_) { ++rejected_detector_sets; return false; }
    detector_tone_hz = tone_hz;
    detector_block_us = bus;
    return true;
  }
  bool setSubDetector(real tone_hz, int64_t bus) override {
    if (running_) { ++rejected_detector_sets; return false; }
    sub_detector_hz = tone_hz;
    sub_detector_block_us = bus;
    return true;
  }
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

  bool nextSubPower(int64_t* mono_us, real* power) override {
    if (sub_queue_.empty()) return false;
    *mono_us = sub_queue_.front().first;
    *power = sub_queue_.front().second;
    sub_queue_.pop_front();
    return true;
  }

  void pump(int64_t true_utc_us, int64_t mono_us) {
    if (!running_) {
      next_block_true_ = true_utc_us;  // resync on restart
      next_sub_block_true_ = true_utc_us;
      return;
    }
    if (next_block_true_ == 0) next_block_true_ = true_utc_us;
    if (next_sub_block_true_ == 0) next_sub_block_true_ = true_utc_us;

    while (next_block_true_ <= true_utc_us) {
      const int64_t t = next_block_true_;
      // Map this true instant back to device monotonic time.
      const int64_t stamp = mono_us - (true_utc_us - t);
      queue_.push_back({stamp, powerAt(t)});
      next_block_true_ += block_us;
    }

    // The channel exists only while the app has it pointed somewhere.
    if (sub_detector_hz <= 0.0f) {
      next_sub_block_true_ = true_utc_us;
      return;
    }
    while (next_sub_block_true_ <= true_utc_us) {
      const int64_t t = next_sub_block_true_;
      const int64_t stamp = mono_us - (true_utc_us - t);
      sub_queue_.push_back({stamp, subPowerAt(t)});
      next_sub_block_true_ += sub_block_us;
    }
  }

  // The cached claim, as on the device (Esp32WwvSampler keeps exactly this).
  int32_t tunedKhz() const { return tuned_; }

  // Inject a block directly, bypassing the WWV marker generator above.
  //
  // pump() synthesises one thing — a station transmitting a minute marker on a
  // propagating band — which is the right model for the clock and useless for
  // CW, where the tone is keyed by a person and carries text. Rather than teach
  // the marker generator Morse, let a test key the queue itself.
  void pushPower(int64_t mono_us, real power) {
    queue_.push_back({mono_us, power});
  }

 private:
  // Where the audio tap is actually listening: the chip's dial, not this
  // adapter's cache. If the chip is on FM (harvesting RDS, or an operator's
  // music), the tap carries program audio — no 1000 Hz minute marker — however
  // firmly the cache believes it is on 15000.
  int32_t rxKhz() const {
    if (tuner == nullptr) return tuned_;
    return tuner->band == FakeTuner::Band::Am ? tuner->khz : 0;
  }

  bool propagates() const {
    const int32_t rx = rxKhz();
    if (rx == 0) return false;
    for (int32_t b : propagating_bands) {
      if (b == rx) return true;
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

  real subPowerAt(int64_t true_utc_us) const {
    if (!propagates() || !sub_carrier_present) return sub_noise_power;
    // What was being TRANSMITTED at the instant now arriving.
    const int64_t tx = true_utc_us - chain_delay_us;
    int64_t tx_s = tx / 1000000;
    int64_t within = tx - tx_s * 1000000;
    if (within < 0) { within += 1000000; --tx_s; }

    // One frame of symbols per minute, cached — pump() asks tens of times a
    // second and the encode walks the calendar.
    const int64_t minute_epoch = (tx_s / 60) * 60;
    if (minute_epoch != enc_minute_) {
      encodeFrame(wwvTimeFromEpochS(minute_epoch), enc_frame_);
      enc_minute_ = minute_epoch;
    }
    const int sec = (int)(tx_s - minute_epoch);
    int64_t width_us = 0;
    switch (enc_frame_[sec]) {
      case TcSymbol::Zero:   width_us = 170000; break;
      case TcSymbol::One:    width_us = 470000; break;
      case TcSymbol::Marker: width_us = 770000; break;
      default:               width_us = 0;      break;
    }
    const bool on = within >= sub_lead_us && within < sub_lead_us + width_us;
    return on ? sub_tone_power : sub_noise_power;
  }

  bool running_ = false;
  int32_t tuned_ = 0;
  int64_t next_block_true_ = 0;
  int64_t next_sub_block_true_ = 0;
  std::deque<std::pair<int64_t, real>> queue_;
  std::deque<std::pair<int64_t, real>> sub_queue_;
  mutable int64_t enc_minute_ = -1;
  mutable TcSymbol enc_frame_[60] = {};
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
  FakeTuner tuner;   // the ONE dial both adapters below share
  FakeRdsSource rds;
  FakeWwvSampler wwv;
  FakeWiFi wifi;
  FakeStore store;

  Sim() {
    rds.tuner = &tuner;
    wwv.tuner = &tuner;
  }

  // The stock firmware touching the chip directly — selectBand() goes through
  // neither adapter, so their caches go stale exactly as they do on hardware.
  void operatorTune(int32_t khz, bool fm) {
    if (fm) tuner.tuneFm(khz); else tuner.tuneAm(khz);
  }

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
