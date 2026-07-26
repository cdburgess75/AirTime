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

void Esp32WwvSampler::stop() { running_ = false; }

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

  while(!quit_)
  {
    if(!running_)
    {
      armed = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

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
      in_block = 0;
      armed = true;
    }

    if(in_block == 0) block_start_us = esp_timer_get_time();

    const int raw = analogRead(cfg_.adc_gpio);

    // Track and remove the DC bias. Leaving it in would leak into the detector
    // through the filter's sidelobes; the tap sits around 2870 counts here.
    dc += cfg_.dc_alpha * ((real)raw - dc);
    const real x = ((real)raw - dc) / cfg_.adc_full_scale;

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
    }
  }

  finished_ = true;
  vTaskDelete(nullptr);
}

}  // namespace airtime_esp32
