#pragma once
//
// Umbrella header for the ESP32 adapters — see airtime_core.h for why these
// exist (arduino-cli discovers libraries only through src/-root headers).
// Pulling in airtime_core's umbrella first also makes that library's include
// path available to every adapter header below.

#include <airtime_core.h>

#include "airtime_esp32/esp_clock.h"
#include "airtime_esp32/rds_source.h"
#include "airtime_esp32/time_store.h"
#include "airtime_esp32/wifi_control.h"
#include "airtime_esp32/wwv_sampler.h"
