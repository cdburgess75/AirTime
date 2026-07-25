#pragma once
//
// AirTime core — shared types.
//
// This library is deliberately platform-independent: no Arduino, no ESP-IDF.
// It compiles on the host for unit testing and is consumed unchanged by the
// firmware once the hardware seam (ADC, SI4732 RDS registers, esp_timer, NVS)
// is wired up. See docs/ARCHITECTURE.md.

#include <cstdint>

namespace airtime {

// Single-precision throughout the DSP path: the ESP32-S3 has a hardware
// single-precision FPU, and `double` is software-emulated (slow) on it. The
// Goertzel detector runs in real time on core 2, so this matters.
using real = float;

}  // namespace airtime
