# AirTime — Status & Task Tracker

Live checklist for the build. Milestone contents come from [`PLAN.md §7`](PLAN.md#7-milestones).
Legend: ⬜ not started · 🟡 in progress · ✅ done · ⛔ blocked/gate

Last updated: batch 4 adds the hardware seam (`hal.h`), the §5 display
formatting, the WWV phase-correction helper, and **`AirTimeApp`** — the wiring
that makes a device — plus a **fully simulated ATS Mini** (`test/fakes.h`) that
runs the real app end to end. **81 tests / 2151 checks** passing via `make test`.

The simulated device cold-starts from RDS, outvotes a lying station, lets WWV
refine the coarse fix, learns its crystal (27.99 ppm measured against a true
28 ppm), survives a warm boot honestly unsynced, and serves accurate stratum-1
NTP to a simulated laptop — all with WiFi and ADC2 never live together.

> **Milestone 0 is complete (2026-07-25) and the pre-flash gate is lifted.** The device
> has a verified backup, has been erased and restored on purpose, runs a build made from
> this repo, and **has a confirmed audio tap on IO11**. Adapter work against
> [`hal.h`](../lib/airtime_core/src/airtime/hal.h) can now proceed against real hardware.

---

## ✅ Milestone 0 — Safety net & hardware verification — **COMPLETE (2026-07-25)**
**The pre-flash gate is LIFTED.** Recovery drill passed, and the IO11 tap is confirmed as
factory-routed — no jumper wire needed, so PLAN.md §2's standing assumption is retired.

📖 **Full step-by-step runbook: [`MILESTONE0.md`](MILESTONE0.md)** — copy-pasteable commands,
the recovery drill, the IO11 beat test, a results table to fill in, and troubleshooting.

- [x] Identify chip + partition layout ✅ ESP32-S3 rev v0.2, 16 MB flash, 8 MB PSRAM; partitions reach 0x800000
- [x] Back up stock firmware ✅ full 16 MB, verified (`aeb512fe…`). **Kept local only — NOT in the repo**; see decision 2
- [x] **Recovery drill** ✅ **PASSED** — erased and restored on purpose; boots to stock
- [x] Build **stock** `ats-mini` via **Arduino CLI** ✅ built + flashed first try (`esp32s3-ospi`); device boots **ATS-Mini F/W v2.35 Jul 25 2026**
      - [x] §5d verified: **PSRAM 8192k** in Settings→About (OSPI correct), WiFi joins, HF receives
- [x] **IO11 verification** ✅ **TAP CONFIRMED** — measured with our own `AirTimeProbe`, not the
      backlight test (ats-mini has no audio ADC path, so that test cannot work). With the
      9999 USB tone: rms 37→73 and the dominant frequency moves 2750→1250 Hz. **No jumper needed.**
- [x] Record outcomes in the [`MILESTONE0.md §7`](MILESTONE0.md#7-record-the-results) table and commit

> **Two findings for later:** the receiver reads ~250 Hz low at 10 MHz (~25 ppm), and a
> steady ~2750 Hz noise component sits in the receive chain at zero volume (rms ≈ 37
> counts) — that is the floor `wwv_marker` will threshold against. The tuning offset does
> **not** affect real WWV detection, which uses AM envelope recovery rather than an SSB
> beat, so the 1000 Hz Goertzel is correct as written.

## 🟡 Milestone 1 — RDS clock
**Deliverable: self‑setting clock from broadcast FM.**

- [ ] Survey receivable FM stations: PI, whether CT is sent, and **each station's CT offset from truth** — that last one drives the arbiter's phase behaviour, see the weighting note below *(hardware)*
- [x] RDS CT‑group (group 4A) decode — `rds_ct` ✅ host-tested
- [x] Multi‑station **voting** logic — `station_vote` ✅ host-tested (scan is hardware)
- [ ] Minute‑boundary set *(needs disciplined clock — batch 2)*
- [ ] Timezone config
- [x] Persist last‑known date/time to NVS — `NvsTimeStore` ✍️ written, compiles for target; on‑device verify pending

## 🟡 Milestone 2 — Serve
**Deliverable: laptop runs FT8 synced to the radio, no internet.**

- [x] SoftAP up + UDP/123 socket — `Esp32WiFiControl` ✍️ written, compiles for target; on‑device verify pending
- [x] NTP/SNTP responder — `sntp` ✅ host-tested (packet layer; UDP socket is the adapter)
- [x] Client counting — `sntp::ClientCounter` ✅ host-tested
- [x] Unsynchronized flagging (LI=3 / stratum 16; uncertainty published as root dispersion) — `sntp` ✅
- [x] OS client setup documented (README; revisit after real testing)

## 🟡 Milestone 3 — WWV phase lock
**Deliverable: clock disciplines itself from HF with FM absent.**

- [x] Goertzel 1000 Hz detector — `goertzel` ✅ host-tested (core‑2/IO11 wiring is the `Sampler` adapter)
- [x] Minute‑marker detection: duration gate (700–900 ms) + noise‑floor threshold + leading‑edge timestamp — `wwv_marker` ✅ host-tested
- [x] WiFi‑down listen windows (NTP clients coast through) — `scheduler` ✅ host-tested
- [x] Band stepping 5/10/15 MHz with per‑band success + SNR logging and learned band preference — `scheduler` ✅
- [ ] Calibration constant *(genuinely hardware-dependent: measure once on-device, validate via WSJT‑X DT)*

## 🟡 Milestone 4 — Arbiter + confidence
**Deliverable: the full AirTime runtime behavior of §5.**

- [x] Slew/step rules (<500 ms slew; ≥500 ms needs 2 sources or operator confirm) — `arbiter` ✅
- [x] Two‑source requirement for large corrections (own support ≥2, cross-source corroboration, or operator confirm) — `arbiter` ✅
- [x] Drift learning (residual-frequency integrator w/ injected-slew compensation) — `drift` + `arbiter` ✅ (NVS *persistence* still needs `DriftStore` adapter)
- [x] Uncertainty computation (±(elapsed × drift + source unc); sync flag) — `arbiter` ✅ (display is the `Ui` adapter)
- [x] Boot‑time parallel acquisition (RDS vote + WWV band‑step, WiFi down) — `scheduler` ✅ host-tested
- [x] Hourly listen scheduler + operator "listen now"/"serve now" overrides — `scheduler` ✅ host-tested

## ⬜ Milestone 5 — Field acceptance
**Deliverable: cold start → laptop synced → WSJT‑X DT ≈ 0 across an evening.**

- [ ] Battery‑only, external antenna, no infrastructure cold‑start test
- [ ] Evening of decodes: DT column clusters near 0
- [ ] Multi‑day soak validates drift learning

---

## What is left (the core logic is done)

Every remaining item needs either the physical device or a decision from the
owner. The pure logic of the spec is written and host-tested.

**Firmware adapters** — the interfaces are now *defined* in
[`hal.h`](../lib/airtime_core/src/airtime/hal.h) and *faked* in [`fakes.h`](../test/fakes.h),
so each one is a fill-in-the-blank against a contract the tests already exercise:

| Interface | Device implementation | Status |
|---|---|---|
| `IMonotonicClock` | `esp_timer_get_time()` — `esp_clock.h` | ✍️ written |
| `IRdsSource` | SI4732 RDS FIFO via chip-ops seam — `rds_source` | ✍️ written |
| `IWwvSampler` | ADC2_CH0 on IO11 + our `Goertzel`, task on core 0 — `wwv_sampler` | ✍️ written |
| `IWiFiControl` | SoftAP up/down + the UDP/123 socket — `wifi_control` | ✍️ written |
| `ITimeStore` | NVS via Preferences — `time_store` | ✍️ written |
| *(glue)* | `AirTimeMode.cpp` in the sketch, `-DAIRTIME` builds | ✍️ written |
| *(not an interface)* | TFT + encoder → `AirTimeApp::displayState` / operator calls | ⬜ next |

All of the above **compiles and links for the esp32s3 target** (verified in the dev
container — see "Firmware integration" below). None of it has run on the device yet.

**Genuinely hardware-dependent** — the calibration constant (§4: SI4732 DSP group
delay + amp + ADC latency, est. 10–40 ms), the local FM station survey (M1), and
all of Milestone 5 field acceptance. Milestone 0 measured two useful priors for this
work: the receiver reads **~250 Hz low at 10 MHz (~25 ppm)**, and the receive chain
carries a steady **~2750 Hz noise component at rms ≈ 37 ADC counts** with the volume at
zero — the floor `wwv_marker` thresholds against.

**Firmware base** — `esp32-si4732/ats-mini` is vendored at `firmware/ats-mini/` as a
git subtree. It builds with **Arduino CLI**, not PlatformIO as PLAN.md §6 assumed; the
repo's own `platformio.ini` covers the host test build only.

## Firmware integration — WRITTEN AND COMPILING (2026-07-26)

`AirTimeApp` is wired into the ats-mini sketch behind a **`-DAIRTIME`** compile flag
(same pattern as the Milestone 0 probe): without the flag the build is byte-for-byte
stock; with it, `AirTimeMode.cpp` instantiates the app plus all five adapters and
ats-mini hands over RDS polling, WiFi ownership and a slice of every `loop()` pass.
Build commands:

    tools/build_fw.sh                 # in-container (see script header for why)
    arduino-cli compile --clean -e --build-property \
      "compiler.cpp.extra_flags=-DAIRTIME" -p "$PORT" -u ats-mini   # on the Mac

Decisions and facts encoded in this layer, so they are not re-derived later:

- **The vendored diff is tiny and all guarded.** Three `#ifndef AIRTIME` guards in
  `ats-mini.ino` (boot `netInit`, periodic `checkRds`, `netTickTime`) plus two guarded
  hook calls, and one additive accessor in `SI4735-fixed.h` (`getRdsRawGroup`). Stock
  `checkRds` must be off under AIRTIME because each `FM_RDS_STATUS` read *pops* the
  chip's group FIFO — two readers would each see half the groups.
- **RDS arrival stamps are backdated 87.6 ms** (104 bits ÷ 1187.5 bps): IEC 62106
  aligns the minute edge with the *start* of the 4A group, and the chip can only hand
  a group over once it has received all of it. Deterministic physics, corrected in the
  adapter — unlike per-station bias, which stays a survey question.
- **CT groups are gated at BLE ≤ 1 per block** (up to 2 corrected bits), stricter than
  the chip config (≤ 5): a miscorrected block can move a clock or credit the wrong PI.
- **Tuning bypasses ats-mini's band table** (`rx.setFM/setAM` directly): `useBand`
  writes `band->currentFreq`, and a 75 s station rotation through it would thrash the
  settings save path. Cost: the stock UI's frequency readout goes stale in AIRTIME
  builds, and knob tuning is undone at the next retune. Resolved when the §5 UI lands.
- **SI4732 GPIO1 is the FM/AM antenna switch** (G8PTN, `useBand`) — the glue sets it
  per mode; forgetting it means WWV silence.
- **WWV listening forces volume 35** (the IO11 tap is downstream of the DSP volume;
  Milestone 0 calibrated `tone_power` at exactly that level) and restores the user's
  volume after. The radio is audible while listening — accepted for v1. AM bandwidth
  is pinned to 3 kHz so the Milestone 3 level calibration has a reproducible filter.
- **Open questions for the device session**: whether `PIN_AMP_EN` (GPIO10) can mute
  the speaker without killing the tap, and whether an active BLE radio disturbs IO11
  (Milestone 0 measured with BLE idle — leave BLE Off in settings for now).
- **Umbrella headers** `<airtime_core.h>` / `<airtime_esp32.h>` exist solely because
  arduino-cli's library discovery only matches headers at a library's `src/` root —
  the namespaced `airtime/...` headers are invisible to it. Sketch code must include
  an umbrella first; everything else may then use the prefixed paths.

**The dev container now compiles for the target** (arduino-cli + esp32 3.3.11 +
the sketch.yaml-pinned libraries), so firmware changes get compile-checked before
they ever reach the user's Mac. The egress policy blocks `downloads.arduino.cc` and
`espressif.github.io`; setup works around it with the index from the `gh-pages`
mirror, tools/libraries from their github.com homes, a manually placed `ctags`, and
a clearly-labeled local stub for `dfu-util` (an upload-only tool, never run here —
flashing always happens from the Mac). `tools/build_fw.sh` sets `sketch.yaml` aside
during container builds because a present `default_profile` re-resolves dependencies
from the blocked URLs even when `--fqbn` is given.

## Uncertainty-weighted steering — IMPLEMENTED, with a caveat

`TimeFix.uncertainty_us` now weights how far an accepted correction steers the clock:

    gain = our_var / (our_var + source_var)

applied *after* the §4 accept/reject gates, which are unchanged. This makes rule 5's
"uncertainty is first-class" load-bearing and produces §4's tiering with no hardcoded
priority list. Two things it fixed outright:

- **Uncertainty is now reported honestly.** It was pinned at whatever the last source
  claimed — a flat 250 ms — so a coarse fix arriving after a precise one made the device
  report itself *less* certain than it had been. More data cannot make you less certain.
  The posterior `1/sqrt(1/ov + 1/sv)` fixes it, and it feeds the NTP root dispersion.
- **Uncertainty growth now follows the learned crystal** (§4 rule 4) rather than the
  ±20 ppm datasheet spec, via a new `DriftEstimator::residualPpm()`.

Verified: drift still converges exactly (28.00 ppm, ~0 µs error), and with unbiased
stations the clock holds **−4 to −97 ms** across three hours — inside the sub-100 ms
target.

### The caveat: a systematically biased station still drags phase

With one RDS station **220 ms late**, the error sawtooths: WWV fires hourly and snaps it
to −25 ms, then RDS drags it back to −204 ms over the following hour. Mean −148 ms.
Still inside FT8's ~1 s tolerance, but not "DT clusters near zero".

Neither weighting nor throttling RDS resolves it, and the reason is worth recording:
influence is gain × **rate**, and RDS error is **systematic**, not independent. Repeated
samples of a station that is always 220 ms late do not average toward truth the way the
Kalman blend assumes, so frequent coarse fixes keep winning. Growing uncertainty from the
measured residual does not help either, because the oscillation itself inflates that
residual — the loop feeds itself.

**Deliberately not tuned further, because 220 ms is a number we invented.** Real RDS CT
bias is unknown until the Milestone 1 station survey measures it. Tuning a control loop
against a guessed disturbance would be fitting noise. What the survey should capture per
station: PI, whether CT is sent at all, and the **offset of its CT from truth** — the last
being exactly the quantity this behaviour depends on.

If the measured bias turns out to be significant, the principled fix is to stop treating
repeated fixes from one station as independent evidence — floor the posterior at that
station's own accuracy, so N reports from a biased station never make us more certain
than that station is.

## Setup decisions

1. **How the `ats-mini` base lives in git → `git subtree` at `firmware/ats-mini/`.**
   Pulled directly from upstream `esp32-si4732/ats-mini` (no GitHub fork needed unless we
   later contribute back). Rationale: the adapters must *edit* upstream files, so local
   modifications are unavoidable either way. A submodule would split glue code from the
   core across two repos and adds a clone-time failure mode (every remote session starts
   from a fresh clone; a missed `--recursive` yields a silently empty directory).
   Vendoring is simple but strands us against an active upstream. Subtree gives one repo,
   one clone, *and* a real merge path (`git subtree pull`).
   **Done 2026-07-25** — subtree added at `firmware/ats-mini/`.

   *Correction to this entry's original plan: it assumed a vendored `platformio.ini` that
   would pick up `airtime_core` via `lib_extra_dirs`. Upstream has no `platformio.ini` —
   it is an Arduino sketch built with Arduino CLI, configured by `ats-mini/sketch.yaml`.
   Integration will instead place the core's sources where the sketch build sees them.*

2. **Stock‑firmware backup → NEVER committed. Local + private backup only; checksum in repo.**

   **Reversed 2026-07-25 after inspecting the image.** It was briefly committed here, then
   removed and purged from history. The `settings` NVS partition inside a full flash image
   holds **live WiFi credentials** — on this unit `wifissid1` (8 chars) and `wifipass1`
   (10 chars) were populated — and this repository is **public**. Publishing the image
   published those credentials. (The vendor-copyright concern flagged originally was the
   lesser issue; this was the real one.)

   `.gitignore` now blocks `firmware/backup/*.bin`. Only `SHA256SUMS` and an explanatory
   README live there. The lesson generalises: **a full device flash image is credential
   material**, because NVS holds whatever the firmware stored — treat it like a secret,
   not like a build artifact.

