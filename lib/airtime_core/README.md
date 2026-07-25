# airtime_core

The platform-independent heart of AirTime. **No Arduino, no ESP-IDF, no hardware
calls** — every module takes plain values in (samples, block words, timestamps,
offsets) and returns decisions out. That is what lets the whole thing be
unit-tested on a laptop and then compiled *unchanged* into the ATS Mini firmware.

See [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) for how this core
connects to the hardware, and [`../../docs/PLAN.md`](../../docs/PLAN.md) for the
product spec.

## Layout

Sources live in `src/airtime/`, so every include carries an `airtime/` prefix:

```cpp
#include <airtime/app.h>
```

That prefix is not decoration. A flat layout would put `sntp.h` and `types.h` on the
include path, and **lwIP already ships an `sntp.h`** that the ESP32 Arduino core exposes
— the collision would surface as a baffling compile error deep in a device build. The
directory is also what makes this a valid Arduino 1.5-format library
(`library.properties` at the root, recursive compilation under `src/`), so the same tree
serves the host tests and the firmware unchanged.

## Modules

| File | Purpose | Plan ref |
|---|---|---|
| `types.h` | Shared numeric types (`real` = `float`, to match the ESP32-S3 FPU) | — |
| `goertzel.{h,cpp}` | Single-frequency (1000 Hz) power detector for the WWV minute marker | §4, M3 |
| `wwv_marker.{h,cpp}` | Minute-marker gate: duration (700–900 ms) + noise-floor threshold + leading-edge timestamp | §4, M3 |
| `rds_ct.{h,cpp}` | RDS group 4A clock-time decode; MJD ↔ civil date | §4 Tier 1, M1 |
| `station_vote.{h,cpp}` | Multi-station CT voting / outlier rejection | §4 Tier 1, M1 |
| `disciplined_clock.{h,cpp}` | The internal clock: rate + phase steering, slew-not-step | §4 rule 1, M4 |
| `drift.{h,cpp}` | Crystal drift learning (residual-frequency integrator) | §4 rule 4, M4 |
| `arbiter.{h,cpp}` | Multi-source arbiter — slew/step rules, two-source gate, uncertainty (the heart) | §4, M4 |
| `sntp.{h,cpp}` | NTP/SNTP server packets, honest stratum/leap flagging, client counting | §4 rule 5, M2 |
| `scheduler.{h,cpp}` | Boot acquisition, hourly listen windows, WWV band stepping; enforces the WiFi↔ADC2 invariant | §5, §4, M3/M4 |
| `hal.h` | The hardware seam — interfaces the firmware implements and tests fake | — |
| `display.{h,cpp}` | The §5 display lines (UTC + honest uncertainty/status) | §5 |
| `app.{h,cpp}` | `AirTimeApp` — wires every module to the seam; the whole device | §4, §5 |

## Testing

From the repository root:

```sh
make test
```

Builds with plain `g++` and runs the full suite — no PlatformIO, no network.

Every translation unit is also checked against the constraints the device build imposes:

```sh
g++ -std=gnu++17 -Os -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow -Werror \
    -Ilib/airtime_core/src -c lib/airtime_core/src/airtime/*.cpp
```

## Using it from the ats-mini sketch

`arduino-cli` is pointed at this directory as an extra library search path:

```sh
cd firmware/ats-mini
arduino-cli compile --clean -e --libraries ../../lib -p "$PORT" -u ats-mini
```

The library is only compiled in once the sketch actually includes `<airtime/app.h>`.

## Footprint

Measured with `-Os`: `AirTimeApp` is **1,896 bytes** of RAM in total (arbiter 256,
scheduler 184, station voter 776, client counter 200, config 216), and the compiled core
is roughly **13 KB** of code. Against 512 KB of SRAM and a 3 MB app partition, it is
close to free.

`double` is used in the drift estimator and in `DisciplinedClock::utcAt()`. On the
ESP32-S3 that is software-emulated, but `utcAt()` runs at display/NTP rates rather than
in any hot loop, and the drift math runs once per fix — minutes apart. The DSP path is
single-precision (`real`) precisely because it is the part that runs continuously.

Tests live in [`../../test/`](../../test/) and use a tiny dependency-free harness
(`test_framework.h`).

## Design rules

- **Deterministic and pure.** No wall-clock reads, no allocation in hot paths,
  no globals. Time enters as explicit monotonic timestamps.
- **Fixed capacity, zero heap** where it will run on-device (e.g. the voter).
- **Single-precision DSP** (`real`) — `double` is software-emulated on the S3.
- **Honest failure.** Decoders return `bool`; out-of-range input is rejected, not
  clamped.
