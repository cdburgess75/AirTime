#pragma once
//
// IMonotonicClock over esp_timer — AirTime's time base on the ATS Mini.
//
// esp_timer_get_time() returns microseconds since boot from a 64-bit counter
// that does not wrap in any practical lifetime and does not jump when the system
// clock is adjusted. That last property is the whole point: AirTime's disciplined
// clock steers *its own* UTC estimate on top of a monotonic reference, and would
// be corrupted by a time base that could itself be stepped.

#include <esp_timer.h>

#include <airtime/hal.h>

namespace airtime_esp32 {

class EspMonotonicClock : public airtime::IMonotonicClock {
 public:
  int64_t nowUs() const override { return esp_timer_get_time(); }
};

}  // namespace airtime_esp32
