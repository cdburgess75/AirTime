#pragma once
//
// Umbrella header — and the reason it exists is Arduino library discovery.
//
// AirTime's real headers live under an airtime/ prefix (so lwIP's sntp.h and
// ours can coexist on the include path), but arduino-cli only matches a
// library to an #include by headers at the library's src/ root. Including
// <airtime_core.h> is what tells the build this library is in use; after
// that, <airtime/...> includes resolve everywhere in the translation set.
//
// Host code and tests do not need this file — they add the include path
// directly and say <airtime/app.h> and friends.

#include "airtime/app.h"
#include "airtime/arbiter.h"
#include "airtime/disciplined_clock.h"
#include "airtime/display.h"
#include "airtime/drift.h"
#include "airtime/goertzel.h"
#include "airtime/hal.h"
#include "airtime/morse.h"
#include "airtime/nets.h"
#include "airtime/rds_ct.h"
#include "airtime/scheduler.h"
#include "airtime/sntp.h"
#include "airtime/station_vote.h"
#include "airtime/timezone.h"
#include "airtime/types.h"
#include "airtime/wwv_marker.h"
