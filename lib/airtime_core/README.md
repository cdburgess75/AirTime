# airtime_core

The platform-independent heart of AirTime. **No Arduino, no ESP-IDF, no hardware
calls** — every module takes plain values in (samples, block words, timestamps,
offsets) and returns decisions out. That is what lets the whole thing be
unit-tested on a laptop and then compiled *unchanged* into the ATS Mini firmware.

See [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) for how this core
connects to the hardware, and [`../../docs/PLAN.md`](../../docs/PLAN.md) for the
product spec.

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

## Testing

From the repository root:

```sh
make test      # builds with g++ and runs the full suite (no PlatformIO needed)
```

Tests live in [`../../test/`](../../test/) and use a tiny dependency-free harness
(`test_framework.h`).

## Design rules

- **Deterministic and pure.** No wall-clock reads, no allocation in hot paths,
  no globals. Time enters as explicit monotonic timestamps.
- **Fixed capacity, zero heap** where it will run on-device (e.g. the voter).
- **Single-precision DSP** (`real`) — `double` is software-emulated on the S3.
- **Honest failure.** Decoders return `bool`; out-of-range input is rejected, not
  clamped.
