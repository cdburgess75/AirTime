# AirTime Architecture — the hardware seam

AirTime is being built **core-first, host-tested**. The bet (and the reason we
can safely develop without touching the brickable device) is that essentially
all of the interesting behavior is *pure logic* that never needs an ESP32 to be
written or verified. The hardware is pushed to the very edges.

```
   ┌──────────────────────── firmware (device-only, thin) ────────────────────────┐
   │                                                                               │
   │  SI4732 RDS regs ──► RdsSource ──┐                                            │
   │                                  │  uint16 blocks                             │
   │  ADC IO11 (core 2) ──► Sampler ──┼──► ┌───────────────────────────────┐      │
   │                                  │    │        airtime_core           │      │
   │  esp_timer ──► MonotonicClock ───┼──► │  (platform-independent, pure) │──►   │  Display
   │                                  │    │                               │      │  (UTC + ±unc)
   │  NVS ──► DriftStore / TimeStore ─┘    │  goertzel  rds_ct  wwv_marker │      │
   │                                       │  station_vote  drift  sntp    │──►   │  NTP responder
   │  WiFi/SoftAP control ◄────────────────│  disciplined_clock  arbiter   │      │  (SoftAP)
   │           ▲                           │  scheduler                    │      │
   │           └───────────────────────────┴───────────────────────────────┘      │
   │              scheduler owns WiFi up/down — it enforces the ADC2 rule          │
   └───────────────────────────────────────────────────────────────────────────────┘
```

## Two halves

**`lib/airtime_core/src/airtime/` — pure, portable, tested on the host.**
No Arduino/ESP-IDF includes. Deterministic: time enters as explicit monotonic
timestamps; no module reads a wall clock or allocates in a hot path. This is
where the arbiter, the RDS decode/voting, the Goertzel/WWV detection, and the
drift model live. `make test` exercises all of it with a g++ build.

**Firmware adapters — thin, device-only, added after Milestone 0.**
Each is a small shim that turns a hardware fact into a value the core consumes,
or a core decision into a hardware action. Anticipated seam:

| Adapter | Wraps | Feeds / driven by core |
|---|---|---|
| `RdsSource` | SI4732 RDS group registers | → `decodeRdsClockTime()` → `StationVoter` |
| `Sampler` | ADC2_CH0 on IO11 (core 2, WiFi down) | → `Goertzel` → `wwv_marker` |
| `MonotonicClock` | `esp_timer` µs counter | → `disciplined_clock` / `arbiter` |
| `DriftStore` / `TimeStore` | NVS | ↔ learned ppm, last-known date/time |
| `NtpResponder` | lwIP UDP/123 over SoftAP | ← served time + uncertainty/stratum |
| `WiFiControl` | SoftAP up / teardown | ← arbiter's listen-window scheduling |
| `Ui` | TFT + encoder | ← display state; → manual set / operator confirm |

## Why this ordering

- The **ADC2-under-WiFi silicon constraint** (PLAN.md §2) is a *scheduling*
  decision, so it lives in `scheduler` as pure logic: `Directive.wifi_up` and
  `Directive.wwv_listening` are never both true, and a test sweeps 8 simulated
  hours of fixes and operator overrides asserting exactly that. The
  `WiFiControl`/`Sampler` adapters just obey the directive. A constraint that
  would otherwise show up as a baffling on-device failure is a unit test instead.
- The **arbiter** (the heart, §4) is a state machine over `(monotonic_time,
  correction, source, uncertainty)`. Zero hardware. It is the highest-value,
  highest-risk logic, so it gets the most host-side testing before it ever runs
  on the device.
- The **calibration constant** (§4) is the one genuinely hardware-dependent
  number (DSP group delay + amp + ADC latency). It is a single injected offset,
  measured once on-device — not something the core logic can know a priori, and
  deliberately isolated so nothing else depends on hardware timing.

## Testing philosophy

### The simulated device

Because the seam (`hal.h`) is injected, the **entire device runs on the host**.
`test/fakes.h` implements a simulated ATS Mini — a crystal drifting at a
configurable ppm, FM stations transmitting real RDS group 4A clock-time (correctly
or otherwise), WWV minute markers that only arrive on propagating bands, a receive
chain with latency, WiFi, and NVS — and `test/test_app.cpp` drives the **real**
`AirTimeApp` against it. Those tests cover cold start, outvoting a lying station,
WWV refining RDS's coarse fix, drift learning and persistence, warm boot, serving
accurate NTP to a simulated laptop, and the ADC2/WiFi invariant end to end.

This is what makes hardware bring-up cheap: when the adapters are written, the
logic they feed has already been exercised for simulated hours.

> A cautionary note now embedded in the fake: `Sim::advance` derives monotonic
> time from *total* elapsed time, never by accumulating a per-step delta. At 10 ms
> steps a 28 ppm error is 0.28 µs per step, which rounds to zero every time — the
> first version of the fake silently simulated a perfect crystal and made the
> drift-learning test fail against correct application code.

### Coverage

Every core module ships with unit tests that pin its contract (81 cases,
2151 checks; `make test`):
- `goertzel` — tone detection, amplitude scaling, off-frequency rejection.
- `wwv_marker` — 800 ms detection + leading-edge timestamp; short/long/low-power
  rejection (the duration gate).
- `rds_ct` — the RDS-standard MJD anchor (1982-08-06 = MJD 45187), field
  packing, date round-trips, and rejection of malformed groups.
- `station_vote` — consensus, outlier rejection, single-source handling, dedup.
- `disciplined_clock` — rate accuracy, slew-not-step, no discontinuity on rate
  change.
- `drift` — open-loop convergence to the true crystal error, clamping.
- `arbiter` — slew-vs-step thresholds, two-source gate (support / corroboration /
  operator confirm), uncertainty growth, and a **closed-loop** test where a
  25 ppm-slow crystal is learned and the per-fix offset collapses to < 20 ms/hour.
- `sntp` — the NTP epoch anchor, timestamp round-trips, request validation,
  synced (LI 0 / stratum 1 / refid) vs unsynced (LI 3 / stratum 16) responses,
  and bounded client counting.
- `scheduler` — boot acquisition, fix-or-timeout promotion, hourly windows,
  operator overrides, band stepping and learned band preference, plus the
  **WiFi↔ADC2 invariant sweep**.
- `wwvPhaseCorrection` — nearest-minute locking, calibration-constant handling,
  and refusal beyond the acceptance window (it must never guess a minute).
- `display` — the §5 lines verbatim, including the blunt unsynced form.
- `app` — the end-to-end simulations described above.
