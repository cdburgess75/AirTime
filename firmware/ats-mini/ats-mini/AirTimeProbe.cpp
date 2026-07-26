//
// AirTime IO11 probe — Milestone 0 §6 hardware verification.
//
// PLAN.md §2 carries a standing assumption: that the amplifier output is routed
// to GPIO IO11 (ADC2_CH0) on this unit. Everything in Milestone 3 (WWV detection)
// depends on it. The published test is to flash HJBerndt's closed-source binary
// and watch for the backlight flickering in time with a tone — a qualitative
// check that requires trusting a third-party image.
//
// This does the same job quantitatively, in our own code, on the toolchain that
// is already working: it samples IO11 and reports the peak-to-peak swing plus a
// 1000 Hz Goertzel magnitude, so "is audio present on this pin" becomes a number
// you can watch change as you turn the volume knob.
//
// It is compiled ONLY when AIRTIME_IO11_PROBE is defined, so stock behaviour is
// untouched by default:
//
//   arduino-cli compile --clean -e \
//     --build-property "compiler.cpp.extra_flags=-DAIRTIME_IO11_PROBE" \
//     -p "$PORT" -u ats-mini
//
// ==> CRITICAL: WiFi must be OFF while this runs. <==
// ADC2 (which includes IO11) cannot be read while WiFi is active — this is an
// ESP32 silicon limitation, not a firmware choice, and it is the same constraint
// AirTime's scheduler is built around. With WiFi up you will read garbage.
//
// GPIO11 is unused by the ATS Mini build: the parallel display uses 5-9 and
// 38-48, the SI4732 uses 15-18, the amp enable is 10, the encoder is 1/2/21.
// (TFT_MOSI 11 in tft_setup.h belongs to the LilyGo variant's SPI display,
// which is the #if branch we do not compile.)

#ifdef AIRTIME_IO11_PROBE

#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

#define AIRTIME_PROBE_GPIO 11      // ADC2_CH0 — the assumed audio tap
#define AIRTIME_PROBE_WINDOW_MS 200
#define AIRTIME_PROBE_PERIOD_MS 1000
#define AIRTIME_PROBE_MAXN 4096
#define AIRTIME_PROBE_TONE_HZ 1000.0f

static uint16_t probeBuf[AIRTIME_PROBE_MAXN];
static bool probeInit = false;

void airtimeIo11Probe()
{
  static uint32_t lastRun = 0;
  const uint32_t now = millis();
  if(now - lastRun < AIRTIME_PROBE_PERIOD_MS) return;
  lastRun = now;

  if(!probeInit)
  {
    analogReadResolution(12);
    // Full 0-3.3 V span: the tap rides on a DC bias, so we want headroom
    // both sides rather than a clipped window.
    analogSetPinAttenuation(AIRTIME_PROBE_GPIO, ADC_11db);
    probeInit = true;
    Serial.println("AIRTIME-IO11 probe active. WiFi MUST be off (ADC2 limitation).");
    Serial.println("AIRTIME-IO11 compare pp/g1k with volume DOWN vs volume UP.");
  }

  const int64_t t0 = esp_timer_get_time();
  uint32_t n = 0;
  int mn = 4095, mx = 0;
  double sum = 0.0;

  while((esp_timer_get_time() - t0) < (int64_t)AIRTIME_PROBE_WINDOW_MS * 1000 &&
        n < AIRTIME_PROBE_MAXN)
  {
    const int v = analogRead(AIRTIME_PROBE_GPIO);
    probeBuf[n++] = (uint16_t)v;
    if(v < mn) mn = v;
    if(v > mx) mx = v;
    sum += v;
  }

  const int64_t dt = esp_timer_get_time() - t0;
  if(n == 0 || dt <= 0) { Serial.println("AIRTIME-IO11 no samples"); return; }

  const float fs = (float)n * 1e6f / (float)dt;
  const float mean = (float)(sum / (double)n);

  // Goertzel at 1000 Hz over the captured window, DC removed. Normalised so a
  // full-amplitude tone reads as its amplitude in ADC counts.
  float mag = 0.0f;
  if(fs > 2.5f * AIRTIME_PROBE_TONE_HZ)
  {
    const float w = 2.0f * (float)M_PI * AIRTIME_PROBE_TONE_HZ / fs;
    const float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for(uint32_t i = 0 ; i < n ; i++)
    {
      const float x = (float)probeBuf[i] - mean;
      const float s0 = x + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    mag = (p > 0.0f) ? (2.0f * sqrtf(p) / (float)n) : 0.0f;
  }

  Serial.printf(
    "AIRTIME-IO11 n=%4u fs=%6.0fHz dc=%4.0f min=%4d max=%4d pp=%4d g1k=%6.1f\n",
    n, fs, mean, mn, mx, mx - mn, mag);
}

#endif  // AIRTIME_IO11_PROBE
