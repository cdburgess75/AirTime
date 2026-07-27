#pragma once
//
// IWwvSampler over ADC2_CH0 (GPIO11) — PLAN.md §4.
//
// Runs the core's own airtime::Goertzel against the audio tap on a dedicated
// task, and hands the main loop a stream of (timestamp, normalised power) block
// estimates. Everything downstream — the marker gate, the phase correction, the
// arbiter — is host-tested code in airtime_core; this file exists only to get
// real samples into it.
//
// ── Constraints this class is built around ──────────────────────────────────
//
// **ADC2 cannot be read while WiFi is active.** ESP32 silicon, not a firmware
// choice (PLAN.md §2). The Scheduler guarantees WiFi is down whenever it asks
// for listening, and the ordering is enforced in AirTimeApp::applyDirective.
// Measured consequence of getting it wrong, on the owner's unit: with WiFi up
// the ADC read peak-to-peak 1000-2700 counts with the top rail clipped and a
// wandering DC; with WiFi down, 250-600 counts, no clipping, DC stable to ±3.
//
// **Which core.** PLAN.md says "core 2", meaning the second core. On Arduino-
// ESP32 the main loop already runs on core 1 (APP_CPU), so the sampler defaults
// to **core 0** to actually get a core to itself.
//
// **Timestamps are what matter.** The block timestamp is taken at the *first
// sample* of the block, because the marker detector times the leading edge of
// the tone and that edge is what disciplines the clock. Block length therefore
// sets the edge quantisation: ~20 ms is a good balance against the sub-100 ms
// phase alignment WWV is there to provide.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <airtime/goertzel.h>
#include <airtime/hal.h>
#include <airtime/types.h>

namespace airtime_esp32 {

// Called to retune the receiver. Supplied by the integration layer so this
// adapter needs no knowledge of the host firmware's radio state machine.
using TuneFn = void (*)(int32_t khz, void* ctx);

struct WwvSamplerConfig {
  int adc_gpio = 11;              // ADC2_CH0 — the factory audio tap
  airtime::real tone_hz = 1000.0f;
  int64_t block_us = 20000;       // ~20 ms blocks => ~20 ms edge quantisation
  int core = 0;                   // main loop owns core 1; take the other one
  int task_priority = 2;
  std::size_t queue_len = 256;    // ~5 s of blocks: rides out main-loop stalls
                                  // (flash writes, UI work) without dropping
  // Samples are scaled by 1/adc_full_scale so a full-swing tone reads ~1.0,
  // matching the normalisation airtime::Goertzel and WwvMarkerConfig expect.
  airtime::real adc_full_scale = 2048.0f;
  airtime::real dc_alpha = 0.02f; // EMA tracking the DC bias (~2870 counts here)
};

class Esp32WwvSampler : public airtime::IWwvSampler {
 public:
  Esp32WwvSampler() = default;
  ~Esp32WwvSampler() override;

  // Must be called once before use. `tune` may be null (tuneKhz becomes a no-op,
  // useful when the host firmware is driving the dial).
  bool begin(const WwvSamplerConfig& cfg, TuneFn tune = nullptr, void* ctx = nullptr);
  void end();

  // --- IWwvSampler ---------------------------------------------------------
  void tuneKhz(int32_t khz) override;
  void start() override;
  void stop() override;
  bool isRunning() const override;
  bool nextPower(int64_t* mono_us, airtime::real* power) override;

  // Repoint the DETECTOR — the Goertzel bin and the block length. Not the
  // receiver: tuneKhz() does that, and this deliberately does not touch it.
  //
  // WWV wants 1000 Hz in 20 ms blocks; CW wants a ~700 Hz beat note in 5 ms
  // ones, because a 40 WPM dit is 30 ms long and 20 ms blocks cannot resolve
  // it (morse.h). The task re-reads this config every time it re-arms, which
  // happens on every start(), so a change made while stopped is picked up with
  // no task restart and no window where cfg_ is read from two cores at once.
  //
  // Returns false if called while running, which would be exactly that race.
  bool setDetector(airtime::real tone_hz, int64_t block_us);
  airtime::real toneHz() const { return cfg_.tone_hz; }

  // --- Diagnostics (not part of the interface) -----------------------------
  // Published by the sampler task for observation only; never used for control,
  // so a torn read across cores is harmless.
  airtime::real sampleRateHz() const { return sample_rate_hz_; }
  airtime::real dcLevel() const { return dc_; }
  uint32_t blocksProduced() const { return blocks_; }
  uint32_t blocksDropped() const { return dropped_; }

 private:
  struct Sample {
    int64_t mono_us;
    airtime::real power;
  };

  static void taskEntry(void* self);
  void run();
  airtime::real measureSampleRate();

  WwvSamplerConfig cfg_;
  TuneFn tune_ = nullptr;
  void* tune_ctx_ = nullptr;

  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  // Control flags genuinely shared between the main loop and the sampler task.
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> finished_{false};
  // True while the task is parked outside the sampling loop — i.e. guaranteed
  // not inside an analogRead on ADC2. stop() waits for this before returning,
  // because the caller's next act is typically to start the WiFi radio that
  // ADC2 contends with.
  std::atomic<bool> parked_{true};
  bool begun_ = false;

  int32_t tuned_khz_ = 0;
  airtime::real sample_rate_hz_ = 0.0f;
  airtime::real dc_ = 0.0f;
  uint32_t blocks_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace airtime_esp32
