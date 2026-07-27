#include "wwv_sampler.h"

#include <Arduino.h>
#include <esp_timer.h>

namespace airtime_esp32 {

using airtime::Goertzel;
using airtime::real;

Esp32WwvSampler::~Esp32WwvSampler() { end(); }

bool Esp32WwvSampler::begin(const WwvSamplerConfig& cfg, TuneFn tune, void* ctx)
{
  if(begun_) return true;
  cfg_ = cfg;
  tune_ = tune;
  tune_ctx_ = ctx;

  queue_ = xQueueCreate(cfg_.queue_len, sizeof(Sample));
  if(queue_ == nullptr) return false;

  analogReadResolution(12);
  // Full 0-3.3 V span. The tap rides on a DC bias near mid-rail, so we want
  // headroom both sides rather than a window that clips on peaks.
  analogSetPinAttenuation(cfg_.adc_gpio, ADC_11db);

  quit_ = false;
  running_ = false;
  finished_ = false;
  if(xTaskCreatePinnedToCore(&Esp32WwvSampler::taskEntry, "airtime_wwv", 4096, this,
                             cfg_.task_priority, &task_, cfg_.core) != pdPASS)
  {
    vQueueDelete(queue_);
    queue_ = nullptr;
    return false;
  }

  begun_ = true;
  return true;
}

void Esp32WwvSampler::end()
{
  if(!begun_) return;
  running_ = false;
  quit_ = true;
  // Let the task observe the flag and delete itself before its queue goes away.
  for(int i = 0 ; i < 50 && !finished_ ; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if(!finished_ && task_ != nullptr) vTaskDelete(task_);
  task_ = nullptr;
  if(queue_ != nullptr) { vQueueDelete(queue_); queue_ = nullptr; }
  begun_ = false;
}

bool Esp32WwvSampler::setDetector(real tone_hz, int64_t block_us)
{
  if(running_) return false;
  if(tone_hz <= 0.0f || block_us <= 0) return false;
  cfg_.tone_hz = tone_hz;
  cfg_.block_us = block_us;
  return true;
}

bool Esp32WwvSampler::enableSpectrum(real f0_hz, real df_hz)
{
  if(running_) return false;
  if(f0_hz <= 0.0f || df_hz <= 0.0f) return false;
  spec_f0_hz_ = f0_hz;
  spec_df_hz_ = df_hz;
  spec_frames_ = 0;
  spectrum_on_ = true;
  return true;
}

bool Esp32WwvSampler::disableSpectrum()
{
  if(running_) return false;
  spectrum_on_ = false;
  return true;
}

uint32_t Esp32WwvSampler::copySpectrum(real out[kSpectrumBins]) const
{
  for(std::size_t i = 0 ; i < kSpectrumBins ; i++) out[i] = spec_frame_[i];
  return spec_frames_;
}

void Esp32WwvSampler::tuneKhz(int32_t khz)
{
  tuned_khz_ = khz;
  if(tune_ != nullptr) tune_(khz, tune_ctx_);
}

void Esp32WwvSampler::start()
{
  if(!begun_) return;
  if(queue_ != nullptr) xQueueReset(queue_);
  running_ = true;
}

void Esp32WwvSampler::stop()
{
  running_ = false;
  // Do not return while the task might still be inside an analogRead: ADC2 and
  // the WiFi radio contend in silicon (PLAN.md §2 — the same constraint that
  // forces the teardown ordering), and this adapter's caller brings WiFi up as
  // its very next act. The task parks within a couple of samples of observing
  // the flag; wait for that, bounded so a wedged task cannot hang the loop.
  for(int i = 0 ; i < 25 && begun_ && !parked_ ; i++) vTaskDelay(pdMS_TO_TICKS(2));
}

bool Esp32WwvSampler::isRunning() const { return running_; }

bool Esp32WwvSampler::nextPower(int64_t* mono_us, real* power)
{
  if(queue_ == nullptr) return false;
  Sample s;
  if(xQueueReceive(queue_, &s, 0) != pdTRUE) return false;
  if(mono_us != nullptr) *mono_us = s.mono_us;
  if(power != nullptr) *power = s.power;
  return true;
}

real Esp32WwvSampler::measureSampleRate()
{
  // analogRead's throughput is not a documented constant, so measure it rather
  // than assume one — the Goertzel coefficient depends on it directly.
  const int64_t t0 = esp_timer_get_time();
  const int kProbe = 2000;
  double acc = 0.0;
  for(int i = 0 ; i < kProbe ; i++) acc += analogRead(cfg_.adc_gpio);
  const int64_t dt = esp_timer_get_time() - t0;
  dc_ = (real)(acc / kProbe);
  if(dt <= 0) return 0.0f;
  return (real)kProbe * 1e6f / (real)dt;
}

void Esp32WwvSampler::taskEntry(void* self)
{
  static_cast<Esp32WwvSampler*>(self)->run();
}

void Esp32WwvSampler::run()
{
  bool armed = false;
  Goertzel goertzel(1.0f, 1.0f, 1);   // replaced once the rate is known
  real dc = 0.0f;                     // local: avoids volatile compound-assign
  uint32_t blocks = 0, dropped = 0;
  std::size_t block_n = 0;
  int64_t block_start_us = 0;
  std::size_t in_block = 0;
  // Blocks per yield, recomputed on re-arm from whatever block length is in
  // force. See the vTaskDelay below for why this is a cadence and not a count.
  int yield_every = 1, since_yield = 0;

  while(!quit_)
  {
    if(!running_)
    {
      parked_ = true;   // guaranteed: no ADC activity until running_ again
      armed = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    parked_ = false;

    if(!armed)
    {
      const real fs = measureSampleRate();
      if(fs < 2.5f * cfg_.tone_hz)
      {
        // Too slow to represent the tone at all. Almost always means WiFi is up
        // and ADC2 is unreadable; do not emit numbers that look like data.
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      sample_rate_hz_ = fs;
      dc = dc_;
      block_n = (std::size_t)((double)cfg_.block_us * (double)fs / 1e6);
      if(block_n < 16) block_n = 16;
      goertzel = Goertzel(fs, cfg_.tone_hz, block_n);
      if(spectrum_on_)
      {
        // All bins share block_n, so every bin completes on the same sample
        // and one loop pass harvests a coherent frame. Bins above Nyquist are
        // pinned to an inert placeholder rather than aliased into lies.
        for(std::size_t b = 0 ; b < kSpectrumBins ; b++)
        {
          const real f = spec_f0_hz_ + spec_df_hz_ * (real)b;
          spec_bank_[b] = (f < fs * 0.45f) ? Goertzel(fs, f, block_n)
                                           : Goertzel();
        }
      }
      yield_every = (int)((20000 + cfg_.block_us - 1) / cfg_.block_us);
      if(yield_every < 1) yield_every = 1;
      since_yield = 0;
      in_block = 0;
      armed = true;
    }

    if(in_block == 0) block_start_us = esp_timer_get_time();

    const int raw = analogRead(cfg_.adc_gpio);

    // Track and remove the DC bias. Leaving it in would leak into the detector
    // through the filter's sidelobes; the tap sits around 2870 counts here.
    dc += cfg_.dc_alpha * ((real)raw - dc);
    const real x = ((real)raw - dc) / cfg_.adc_full_scale;

    // Spectrum duty: feed the bank instead of the single detector. The frame
    // is published in place and nothing is queued — no consumer exists for a
    // sample stream in this mode, and a full queue would only pollute the
    // dropped-blocks diagnostic that WWV listening relies on.
    if(spectrum_on_)
    {
      ++in_block;
      bool frame_done = false;
      for(std::size_t b = 0 ; b < kSpectrumBins ; b++)
      {
        real p = 0.0f;
        if(spec_bank_[b].process(x, &p))
        {
          spec_frame_[b] = p;
          frame_done = true;
        }
      }
      if(frame_done)
      {
        ++spec_frames_;
        in_block = 0;
        blocks_ = ++blocks;
        if(++since_yield >= yield_every)
        {
          since_yield = 0;
          vTaskDelay(1);
        }
      }
      continue;
    }

    real power = 0.0f;
    ++in_block;
    if(goertzel.process(x, &power))
    {
      Sample s;
      // Timestamp the START of the block: the marker detector times the leading
      // edge of the tone, and that edge is what disciplines the clock.
      s.mono_us = block_start_us;
      s.power = power;
      if(xQueueSend(queue_, &s, 0) != pdTRUE) ++dropped;
      else ++blocks;
      in_block = 0;
      dc_ = dc;              // publish diagnostics once per block, not per sample
      blocks_ = blocks;
      dropped_ = dropped;

      // One tick of air, or the idle task on this core starves and the task
      // watchdog reboots the chip (observed on first boot: abort ~16 s in,
      // "IDLE0 ... CPU 0: airtime_wwv"). The pause sits BETWEEN blocks, so
      // within-block sample spacing — what the Goertzel bin and the measured
      // rate describe — is untouched, and durations come from block timestamps,
      // which keep counting through the gap.
      //
      // Yield on a fixed CADENCE rather than every block. A tick is 1 ms, which
      // is 5% of a 20 ms WWV block and was fine — but 20% of a 5 ms CW block,
      // and that fifth of the air is not merely lost, it is lost in slices
      // shorter than the element edges the decoder is trying to time. Holding
      // the interval at ~20 ms of audio keeps the idle task exactly as well fed
      // as it was proven to need, whatever the block size.
      if(++since_yield >= yield_every)
      {
        since_yield = 0;
        vTaskDelay(1);
      }
    }
  }

  finished_ = true;
  vTaskDelete(nullptr);
}

}  // namespace airtime_esp32
