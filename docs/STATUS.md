# AirTime — Status & Task Tracker

Live checklist for the build. Milestone contents come from [`PLAN.md §7`](PLAN.md#7-milestones).
Legend: ⬜ not started · 🟡 in progress · ✅ done · ⛔ blocked/gate

Last updated 2026-07-27. **161 tests / 6194 checks** via `make test`.

**The clock works.** Milestones 0-4 are done and Milestones 2 and 3 are verified
over the air: `sntp 192.168.4.1` → **−0.005106 ± 0.070630**, no internet, no GPS.
What remains is field acceptance (Milestone 5) and the things that make it
pleasant to own.

A **fully simulated ATS Mini** (`test/fakes.h`) runs the real app end to end: it
cold-starts from RDS, outvotes a lying station, lets WWV refine the coarse fix,
learns its crystal (27.99 ppm measured against a true 28 ppm), survives a warm
boot honestly unsynced, hands the dial to an operator and takes it back, and
serves accurate stratum-1 NTP to a simulated laptop — all with WiFi and ADC2
never live together.

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

- [x] Survey receivable FM stations — **DONE 2026-07-26**, New Orleans home QTH, two runs ~35 min apart. Verdict: **offsets are stable per station** (earned-trust assumption holds), the dial splits into one on-time station, a 0.9–3.2 s "sloppy" crowd that still asserts the correct minute, and broken clocks minutes-to-hours off. Warm-start list (89.9/104.7/107.5) baked into `AirTimeMode.cpp`. Full table:

  | MHz | PI | Station | offset (runs) | class |
  |---|---|---|---|---|
  | 89.9 | A920 | WWNO | +37/+84/+87 ms | **on time — the anchor** |
  | 104.7 | 6E47 | WJSH | +216 ms (n=1) | good |
  | 107.5 | 33CB | K-LOVE | +352/+367 ms | good |
  | 98.9 | 8B94 | WUUU | +0.91/+1.00 s | sloppy, stable |
  | 100.7 | 8774 | (Eagle) | +0.93 s | sloppy |
  | 98.1 | 5D3B | — | +1.50/+1.56 s | sloppy, stable |
  | 91.1 | 82F3 | Vida | +1.59 s | sloppy |
  | 97.7 | 6373 | — | +2.25 s | sloppy |
  | 103.3 | 833C | — | +2.35 s | sloppy |
  | 94.3 | 87BB | — | +2.39 s | sloppy |
  | 106.1 | 829D | — | +2.69 s | sloppy |
  | 101.9 | 72F2 | — | +2.75 s | sloppy |
  | 106.5 | 3CA1 | — | +2.98 s | sloppy |
  | 94.1 | A687 | — | +2.6/+3.2/+3.1 s | sloppy |
  | 107.1 | 685F | — | +3.11/+3.19 s | sloppy, stable |
  | 105.3 | 9989 | WWL | +10.3 s | broken |
  | 92.3 | 986D | Alt 92.3 | +212.2/+212.3 s | broken (~3.5 min) |
  | 96.5 | 8776 | WTGG | +484.7/+484.9 s | broken (~8 min) |
  | 90.7 | A945 | WWOZ | +747.3/+747.4/+747.5 s | broken (~12.5 min, free-running) |
  | 97.1 | F000 | — | +22366.5/+22366.6 s | broken (6.21 h, stable!) |
  | 100.1 | 186C | — | (ble=0111 sample decoded to year 2049) | poison — see report filter |

  ~15 stations agree on the correct minute within ±3.2 s while broken clocks agree with nobody — the minute-consensus design is confirmed by data. Sloppy-station offsets repeat within ~0.5 s run-to-run; tight ones within 100 ms.
- [x] RDS CT‑group (group 4A) decode — `rds_ct` ✅ host-tested
- [x] Multi‑station **voting** logic — `station_vote` ✅ host-tested (scan is hardware)
- [x] Minute‑boundary set ✅ the clock is disciplined and serving; `sntp` read **−0.005106 ± 0.070630** on device
- [x] Timezone config ✅ `timezone` — eight zones as DST *rules*, host-tested against the real 2026 transitions; Settings → Time Zone, persisted in NVS
- [x] Persist last‑known date/time to NVS — `NvsTimeStore` ✅ **verified on device**: a warm boot restores the time and correctly reports itself UNSYNCED until a real source speaks

## 🟢 Milestone 2 — Serve — **VERIFIED ON DEVICE (2026-07-26)**
**Deliverable: laptop runs FT8 synced to the radio, no internet.**

First end-to-end serve: the owner's Mac joined the AirTime AP and ran
`sntp 192.168.4.1` → **`-0.343 s ± 0.154 s`**. The −343 ms offset is exactly
the surveyed RDS station-lateness envelope (+80..+360 ms stations pull the
clock behind truth by a few hundred ms), and the ±154 ms bound carries the
device's own published root dispersion — the honesty path works. Already well
inside FT8 tolerance; the first WWV listen window should tighten it to tens
of ms. Remaining below is WSJT-X itself.

- [x] SoftAP up + UDP/123 socket — `Esp32WiFiControl` ✅ **verified on device**
- [x] NTP/SNTP responder — `sntp` ✅ host-tested (packet layer; UDP socket is the adapter)
- [x] Client counting — `sntp::ClientCounter` ✅ host-tested
- [x] Unsynchronized flagging (LI=3 / stratum 16; uncertainty published as root dispersion) — `sntp` ✅
- [x] OS client setup documented (README; revisit after real testing)

## 🟢 Milestone 3 — WWV phase lock — **VERIFIED OVER THE AIR (2026-07-26)**
**Deliverable: clock disciplines itself from HF with FM absent.**

**The reading: `sntp 192.168.4.1` → `-0.005106 ± 0.070630`.** Five
milliseconds against the laptop's own NTP-disciplined clock, no internet, no
GPS. The log for the same run: `sources RDS+WWV`, `fix[wwv +207ms pair=n A]`
(marker detected on 15 MHz, correction under the step threshold, applied
directly), `mk=4`, base uncertainty ≈30 ms. The same evening the three FM
stations drifted to **−728 ms** consensus lateness — double the survey — and it
no longer mattered: WWV owns the phase, and uncertainty weighting + the RDS
throttle keep the biased crowd to a whisper.

The −5 ms residual bounds the receive-chain calibration constant: it is
smaller than one sntp reading's noise. `wwv_calibration_us = 0` stands until a
multi-reading average says otherwise — a refinement, not a blocker.

- [x] Goertzel 1000 Hz detector — `goertzel` ✅ host-tested (core‑2/IO11 wiring is the `Sampler` adapter)
- [x] Minute‑marker detection: duration gate (700–900 ms) + noise‑floor threshold + leading‑edge timestamp — `wwv_marker` ✅ host-tested
- [x] WiFi‑down listen windows (NTP clients coast through) — `scheduler` ✅ host-tested
- [x] Band stepping 5/10/15 MHz with per‑band success + SNR logging and learned band preference — `scheduler` ✅ (holds a band that is producing markers; the dwell bounds patience with a *silent* band)
- [x] Markers detected over the air ✅ (`run=798–800 ms`, tone ~400× floor, ticks duration-rejected)
- [x] Marker → accepted fix chain ✅ host-tested (consecutive-marker self-corroboration; see "three bugs in one chain")
- [x] **Accepted fix verified ON DEVICE** ✅ `fix[wwv +207ms pair=n A | rds −728ms n=3 A]`, and `sntp` −0.005106 ± 0.070630 immediately after
- [ ] Calibration constant *(genuinely hardware-dependent: measure once on-device, validate via WSJT‑X DT)*

## 🟡 Milestone 4 — Arbiter + confidence
**Deliverable: the full AirTime runtime behavior of §5.**

- [x] Slew/step rules (<500 ms slew; ≥500 ms needs 2 sources or operator confirm) — `arbiter` ✅
- [x] Two‑source requirement for large corrections (own support ≥2, cross-source corroboration, or operator confirm) — `arbiter` ✅
- [x] Drift learning (residual-frequency integrator w/ injected-slew compensation, **measured per source** — cross-source differencing turns a phase bias into a fake rate) — `drift` + `arbiter` ✅
- [x] Uncertainty computation (±(elapsed × drift + source unc); sync flag) — `arbiter` ✅ (display is the `Ui` adapter)
- [x] Boot‑time parallel acquisition (RDS vote + WWV band‑step, WiFi down) — `scheduler` ✅ host-tested
- [x] Hourly listen scheduler + operator "listen now"/"serve now" overrides — `scheduler` ✅ host-tested

## ⬜ Milestone 5 — Field acceptance
**Deliverable: cold start → laptop synced → WSJT‑X DT ≈ 0 across an evening.**

- [ ] Battery‑only, external antenna, no infrastructure cold‑start test
- [ ] Evening of decodes: DT column clusters near 0
- [ ] Multi‑day soak validates drift learning

---

## What is actually left (2026-07-27)

Nothing here blocks the clock; it keeps and serves time correctly today.

**Needs the radio outdoors — Milestone 5, the real remaining work**

📖 **Runbook: [`MILESTONE5.md`](MILESTONE5.md)** — what to check before leaving,
what to record, how to read a failure, and `tools/dt_report.py`, which turns an
evening of WSJT-X decodes into the one number this milestone turns on.

- [ ] Battery-only cold start on an external antenna, no infrastructure
- [ ] An evening of FT8: does the WSJT-X **DT column cluster near zero**? This is
      the acceptance test the whole project is for, and the only one that can
      fail in a way the bench cannot predict.
- [ ] Multi-day soak — does drift learning hold across temperature swings?

**Refinements, in rough order of value**
- [ ] **Time-of-day WWV band choice.** The band list {15, 10, 5} MHz is tuned for
      daytime propagation. A night cold-start begins on a band that is dead after
      dark and has to fail its way down. The clock knows the hour; it should use it.
- [ ] **Web config page** — time zone, nets, stations — so a field change does not
      mean scrolling an encoder. The status page is the read half of this.
- [ ] **Status page polish.** Functional, not yet good-looking.
- [ ] **W1AW CW schedule**, if wanted. The placeholder entries were removed rather
      than shipped wrong; adding them back needs the real published times.

**Not blocking, and bounded**
- `wwv_calibration_us = 0`. The −5 ms residual above bounds the receive-chain
  delay below one sntp reading's noise, so this is a refinement a multi-reading
  average could make, not an open question.
- SkyWave's live NetLogger feed is not applicable on-device: the radio runs a
  bare SoftAP with no route to the internet, by design. It would be a `tools/`
  import at most.

## The single-tuner redesign (2026-07-27, after the retrospective)

The recurring bug class of this project — a chip pointed somewhere else than
the software believes — existed because the test fakes were two independent
radios while the device has ONE tuner. Three structural changes closed it:

- **`FakeTuner`** — one shared dial in the simulation. The adapters' `tunedKhz()`
  values remain what they are on hardware: *cached claims* about a chip someone
  else may have moved. Tests now assert on the truth, the claim, and the
  difference, which is the bug class itself.
- **`OpMode` enum** — Clock/Radio/Cw as one state machine in `AirTimeApp`, with
  every transition sequenced in `setMode()` where the fakes exercise it. The
  firmware's mode switch no longer contains call ordering whose comment
  admitted it was load-bearing; the detector repointing (`setDetector`) is part
  of the `IWwvSampler` contract now.
- **`pickDialBand`** — the narrowest-band choice moved from untested glue into
  `airtime/dial.h`, host-tested against a copy of the real shipped band table
  (the "ALL swallows every net" trap is pinned by test).

The shared tuner exposed **two real device bugs within minutes of existing**:

1. **Single-station RDS deafness.** Nothing retuned the chip to FM after a WWV
   listen window — the dwell rotation requires >1 station — so a one-station
   config sat parked on AM forever, coasting while the station list looked
   healthy. Fixed: window end hands the dial back (`applyDirective`).
2. **Deaf listen windows.** With that retune in place, `tuned_wwv_khz_` went
   stale instead: the next window wanted the band the cache already claimed,
   skipped the retune, and spent three minutes sampling FM program audio. This
   is almost certainly the field mystery of "music on 10 MHz" — the mkr line
   printed the wanted band while the chip sat on a local FM station, and the
   inflated floor read as jamming. Fixed: every FM tune goes through
   `tuneRds()`, which voids the WWV claim; a listen window can no longer open
   without genuinely retuning.

All three invariants are sabotage-verified (re-introducing each bug fails the
suite). One harness lesson learned the hard way: a test that dereferences a
pointer it just reported null takes the binary down before the summary prints,
and a runner grepping for FAIL lines reads that wreck as a pass — the bias test
now returns after reporting, and verification runs read the summary line, not a
filter.

The Menu.cpp restructure followed (same day): the AirTime menus are now
`{label, cmd}` **tables** (`atRootMenu`/`atSettingsMenu`), activation keys off
the globally-unique command instead of a row's position, and the stock
`menu[]`/`settings[]` arrays and index defines returned to pristine upstream
text in both builds — serving only as panel-title strings, with the stock
binary byte-identical as proof the restructure touched nothing else. The
label/position/command triple that produced three silent bugs no longer has a
second structure to disagree with. Remaining from the retrospective: only the
cosmetic `IWwvSampler` rename.

## How the adapters got here (historical)

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
| *(not an interface)* | TFT + encoder → `AirTimeApp::displayState` / operator calls | ✅ on device |

All of the above **runs on the device** — Milestones 2 and 3 are verified over the air.
The dev container compile-checks every change before it reaches the owner's Mac.

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

**If `arduino-cli: command not found` after a container is reclaimed:** only the
binary is lost. `~/.arduino15` (platform + index) and `~/Arduino/libraries` are on
persistent storage and survive, so there is nothing to re-download but the ~17 MB
executable itself — and the upstream `install.sh` cannot fetch it here, because it
resolves through `downloads.arduino.cc`. Take it from GitHub directly:

```sh
curl -sSL -o /tmp/acli.tgz \
  https://github.com/arduino/arduino-cli/releases/download/v1.2.2/arduino-cli_1.2.2_Linux_64bit.tar.gz
tar -xzf /tmp/acli.tgz -C /tmp arduino-cli
install -m755 /tmp/arduino-cli /usr/local/bin/arduino-cli
```

`/usr/local/bin` rather than `~/bin` — the latter is not on the persistent volume,
which is how the binary went missing in the first place.

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

## First day on hardware — what is verified and what is open (2026-07-26)

Verified on the device, over the air, in one day: RDS cold start to sync in
30 s; uncertainty visibly tightening 250→110 ms across weighted votes and
growing honestly between them; the RDS throttle stretching to its 10-minute
cadence once disciplined; NTP served over the AirTime AP (first measurement
`sntp 192.168.4.1` → −0.343 s ± 0.154 s, exactly the surveyed station-lateness
envelope); **warm boot** restoring time+drift from NVS as `UNSYNCED —
last-known + drift` and correctly starting WWV listening immediately;
fast-build listen windows firing on cadence with the single-tuner rules
visible (RDS counters freeze during windows); the watchdog fix holding
(zero resets across hours); and the serial-backpressure fix (drop=0).

**RESOLVED: WWV markers ARE detected.** The `mkr[]` diag line settled it in
one window: `run=798–800 ms`, `mk=3`, 58 second-ticks correctly rejected by
the duration gate, tone peak `5.2e-2` against a `1.2e-4` floor — about 400×.
Detection was never the problem. Field lesson already paid for: the stale
stock display misled the operator into a false jamming theory (the "9999"
readout was a probe-era leftover; the music was our own FM rotation) — the
§5 display is promoted in priority.

## Why three good markers changed nothing — three bugs in one chain (2026-07-26)

The markers were detected and then thrown away. Each of the three faults
below is individually sufficient to keep WWV out of the clock forever, and
none is visible from the `mkr[]` line — which is why the `fix[]` line now
reports what the *arbiter* did, not just what the detector saw.

**1. A rejected fix still closed the listen window.** The station holding the
clock was biased past the 500 ms step threshold, so every marker implied a
large correction. §4 rule 3 rightly refuses a large step from one source —
but `pollWwv` called `sched_.onWwvFix` unconditionally, so a *rejected* fix
set `has_fix_`, ended the window, and discarded the next minute's marker:
the only evidence that could ever have corroborated it. Split into
`onWwvMarker` (detection → band propagation stats) and `onWwvFix`
(acceptance → end the window).

**2. Nothing could corroborate a WWV fix.** `Arbiter::corroborate()` requires
a *different* source, and on the single-tuner radio no other source exists
during a listen window. But WWV corroborates itself: it transmits an
independent marker every minute. Two markers 50–190 s apart implying the same
correction within ±120 ms are two independent measurements — random audio
that survives the 700–900 ms duration gate lands anywhere in the ±30 s phase
window, so agreeing twice is a ~0.4% coincidence. Such a pair is submitted
with `independent_support = 2`, which the existing rule-3 gate already
accepts. **No arbiter change** — the spec had the door open.

**3. The band rotation stepped mid-measurement.** A fixed 2-minute band dwell
inside a 3-minute window put the retune squarely on the second marker of
every window, and retuning resets the detector, so the tone in flight was
lost (`tone_starts` incremented, `markers` did not). The dwell now bounds
patience with a *silent* band: a band that has produced a marker holds until
the window ends. This is also just better radio — it is the same rule as the
FM dwell gating.

### And a fourth, found by the test written for the first three

With all of the above fixed, WWV got in — and the clock then **lost ~285 ms
an hour**, worse than leaving the crystal alone. The drift estimator was
differencing each fix's offset against *whichever source spoke last*. RDS and
WWV disagree by a constant (the station's bias); dividing a constant by the
seconds between the two sources manufactures a rate error. Measured: a
correctly learned **+17.9 ppm was railed to the ±100 ppm clamp by the first
WWV fix** and stayed there, sawtoothing the clock between −135 and −420 ms —
and that poisoned figure is what gets written to NVS for the next boot.

A frequency error is only observable by watching **one** source's offset
evolve over time. Drift bookkeeping is now per-source (`Arbiter::track_[4]`).
End-to-end result against a station biased 700 ms: converges to **±16 ms and
stays**, rate settles at ~+20 ppm, and the biased station drops out of the
source mask entirely — once the clock is disciplined every correction it asks
for is ≥500 ms, so the arbiter simply stops listening to the liar. Regression
tests: `arb_source_bias_is_not_a_drift`, `app_wwv_pair_corrects_large_rds_bias`.

**Field note:** an accepted correction is *not* an applied one. §4 rule 1
slews rather than steps, and at the 500 ppm ceiling a 700 ms correction takes
~23 minutes to inject. The display walks to the right time; it never jumps.

## The clock was an hour wrong and said ±110 ms (2026-07-26, second session)

The `fix[]` line paid for itself immediately, and not the way anyone expected:
`off=-15245ms`. Not a phase error — a **15-second** one. Three independent
readings of the same log agreed on what had happened:

| evidence | value |
|---|---|
| device UTC vs the logging laptop's stamps | **3765 s behind**, constant |
| WWV implied phase correction | −15.245 s = 3765 s **mod 60** ✔ |
| drift of that correction between two markers 34 min apart | **563 ppm** — the 500 ppm slew ceiling |

So: the marker detector was right, the phase math was right, and RDS was
telling the device the correct time and *being accepted*. The clock simply
could not get there. **A slew is capped at `max_slew_ppm`, so applying an
offset takes `offset × 2000`**: 2 s takes 67 minutes, an hour takes **87 days**.
The device had warm-booted from an NVS time saved an hour earlier, accepted
RDS's entirely correct +3765 s correction, and spent the session creeping
toward it at its ceiling while reporting itself synced to ±110 ms and serving
stratum-1 NTP. It would have done that until October.

Three changes, all in the arbiter:

1. **A correction too large to slew is stepped** (`step_apply_us`, 2 s ≈ 67 min
   of slewing). The accept/reject gates are untouched — two agreeing sources or
   an operator, exactly as before. This only decides *how* an already-trusted
   correction is applied. Below the threshold it still slews, so NTP clients
   never see time run backwards.
2. **A restored clock yields to the first real fix.** What NVS remembers is a
   memory, not a measurement; `isSynced()` is already false for it. Making a
   source *corroborate its way past* yesterday's saved time is defending the
   wrong value — the cold-seed exception applies to a memory too.
3. **Pending correction is reported, not hidden** — added to the displayed and
   NTP-served uncertainty, so "accepted but not yet applied" cannot masquerade
   as accuracy. Deliberately **not** added to `Arbiter::uncertaintyUs()`, which
   also sets the blend gain: folding it in there raises a coarse source's pull
   exactly while a better source's correction is landing, and measurably undid
   every WWV fix within ten minutes.

Regression tests: `arb_unslewable_correction_is_stepped`,
`arb_small_correction_still_slews`, `arb_restored_memory_yields_to_first_fix`,
`app_warm_boot_stale_by_an_hour_recovers` (the field case end to end — verified
to fail without each fix, not merely to pass with it).

**Observability lesson, twice over.** Both bugs were invisible from the status
line: rejected markers look exactly like no markers, and an unapplied
correction looks exactly like a correct clock. The `fix[]` line now reports
what the arbiter *did* with both sources and what it still owes:

```
fix[wwv +687ms pair=Y A | rds -12ms n=3 A | pend=0ms] rate=+18.2ppm
```

`pend` must fall to zero. If it sits, the clock is crawling and is not synced,
whatever the ± says.

### Then WWV locked the clock onto the wrong minute

With the step fix flashed, the laptop read **−3899.72 s ± 0.037**. Two things
in that one line:

* **3899.72 s = 65 minutes minus 0.28 s.** A whole number of minutes. That is
  a WWV phase lock — the clock sitting exactly on a minute boundary, and the
  wrong one.
* **±0.037** is WWV's own uncertainty (30 ms). WWV was the source disciplining
  the clock.

Sequence: warm boot restores a 65-minute-stale time → `isSet()` is true, so the
old gate allowed listening → the one tuner parks on AM for acquisition, where
RDS cannot be read at all → WWV gets there first → it "corrects" the clock by
0.28 s, perfectly, onto the wrong minute → **and that accepted fix marks the
clock as sourced**, which re-arms the corroboration gate against the one source
that knew the date. RDS is then locked out permanently. The device reports
±37 ms while being 65 minutes wrong.

Two gates, because the cost of being wrong here is a confidently wrong clock:

1. **WWV may never establish a minute** (`arbiter.cpp`). Cold clock or warm
   memory alike, a WWV fix is refused until some source that carries a date has
   spoken. §3 already said WWV has no date; this makes the arbiter enforce it
   rather than trusting callers to.
2. **A restored memory does not unlock listening** (`effectiveDirective`, now
   gated on `hasSourceFix()` rather than `isSet()`). This also ends the
   five-minute post-warm-boot WiFi outage: with the tuner left on FM, the first
   RDS vote lands in 60–90 s and acquisition ends on the fix instead of the
   timeout.

Tests: `arb_wwv_cannot_establish_a_minute`, plus the WWV-suppression assertion
inside `app_warm_boot_stale_by_an_hour_recovers`. Both verified to fail with
their gate reverted.

**The pattern in all four bugs of this session:** every one was a case of the
device believing something it had no way to know — a rejected fix counted as a
fix, a phase bias read as a frequency, an accepted correction assumed applied,
and a marker that knows only the minute's edge allowed to assert which minute
it was. The uncertainty machinery of §5 is only honest if every claim behind it
is.

### The last silent liar: the AP adapter (fixed, flash at leisure)

During the successful soak the SSID vanished for the better part of an hour
while the `ap[]` line looked healthy, then returned on its own. Cause:
`Esp32WiFiControl::bringUp()` ignored the return values of `WiFi.mode()` and
`WiFi.softAP()` and set `up_ = true` regardless, so one refused start became an
outage that lasted until the *next listen-window cycle* happened to retry. The
likeliest trigger is the architecture's own founding constraint: bringUp runs
moments after the WWV sampler is told to stop, and `stop()` returned while the
task could still be inside an `analogRead` on ADC2 — which contends with the
WiFi radio in silicon.

Hardened (same one-honesty rule as the arbiter fixes): the sampler now *parks*
before `stop()` returns, so ADC2 is quiet before esp_wifi starts; `bringUp()`
checks its return values, leaves `up_` false on refusal, and retries every
500 ms; `service()` notices a dead AP-mode bit and re-raises; and the status
line prints truth, not intent — `ap[up ...]` / `ap[DOWN* ... afail=N]`. A
silent AP outage is no longer possible: it either self-heals within a second
or the log says exactly why not.

## Built after the clock worked (2026-07-27)

With timekeeping verified, the work moved to what the device is like to *own*.
Each item below is either host-tested or verified in the linked binary.

| | |
|---|---|
| **WWV teaches RDS** | Each station's constant lateness is measured while WWV is fresh, then subtracted. The point is not the milliseconds: a station 700 ms late is *vetoed* once WWV pulls the clock onto the true minute, because every correction it asks for then exceeds the 500 ms gate. Correcting the bias hands the continuous source back. |
| **Learning persists** | Station biases and band propagation survive power-off in NVS. Format is versioned, length-checked and all-or-nothing, because a corrupt blob that decodes *partially* would invent a station bias and poison a working clock. |
| **Time zones as rules** | Eight zones with their own DST behaviour, host-tested against the real 2026 transition instants. A fixed label is wrong half the year; a fixed offset is wrong the other half. |
| **The §5 screen** | Local time as the headline in a 48 px seven-segment face, UTC beneath it, honest dial line, and what is on the air right now. |
| **Daylight theme** | Inverted for direct sun. Dark red and dark green accents — the bright ones that read well on black vanish on white. |
| **AirTime menu** | Time Zone, WWV Band and the §5 operator actions, which had been implemented and tested since Milestone 4 and reachable from nothing. Choices persist in AirTime's own NVS namespace, so the build stays additive to upstream. |
| **Net directory** | "Is it on NOW", answerable only by a radio that knows UTC. Shown solely while synced — a schedule read off a wrong clock looks right, which is worse than showing nothing. |
| **CW decoder** | Core host-tested at 7–35 WPM, and **wired** (2026-07-27): Mode → CW Copy hands over the dial, repoints the sampler's Goertzel to 700 Hz in 5 ms blocks, and turns the panel into a text terminal with a tuning bar. Costs the access point — the audio tap and the WiFi radio cannot share the chip — which the screen states rather than leaving to be discovered. |
| **Release image** | `tools/release.sh` merges bootloader + partitions + application into one file at offset 0. Version on the About screen. |
| **Client sync** | `tools/airtime-sync.sh`, including an install-once agent that tracks the radio when present and does nothing when absent. |

**The pattern worth keeping.** Nearly every bug this project has produced was
the device believing something it had no way to know, and nearly every one was
caught by a test written to reproduce a real log rather than by inspection. The
CW decoder alone yielded three: a noise floor seeded from a sample that turned
out to be a tone (permanently deaf), a unit estimator that could not bootstrap
onto a sender slower than its guess (fluent nonsense, forever), and elements
classified before anything was known about the sender (first letter always
wrong). None would have been visible in review.

## Field variability — the survey is a probe, not the config (2026-07-26)

Owner direction: location, antenna, propagation, and time of day are all
variables — the device goes to the field. A station list baked from a home
survey is therefore a **warm start, not the mechanism**. Combined with what the
first real survey showed (one phase-accurate CT station on the whole New
Orleans dial; a crowd asserting the right minute but 1–3 s sloppy; several
clocks minutes-to-hours broken), the design is:

1. **The device self-surveys.** The FM scan the survey probe does (RSSI/SNR
   pass, then dwell rotation hunting CT) moves into the app proper. Any stored
   station list is only a hint that makes the first fix faster at a known QTH;
   an unknown dial is discovered from nothing.
2. **RDS voting happens at minute-consensus tolerance, not phase tolerance.**
   Field data: real stations rarely agree within 400 ms (only one is even
   *capable* of it here), but honest-plus-sloppy stations agree on the minute
   within a few seconds — five of them in this market — while broken clocks
   agree with nobody. Widen the vote tolerance to seconds; let the cluster
   pick the minute; report RDS uncertainty honestly at seconds scale for
   unvetted stations.
3. **Stations earn phase trust on the device, the same way the survey measured
   it.** Once WWV has disciplined the clock, every CT arrival is a measurement
   of that station's own offset. A station observed stable and tight (an 89.9)
   earns a small uncertainty and helps hold phase between listen windows; a
   sloppy or broken one never does. Per-station stats persist in NVS. This is
   the "floor the posterior at the station's own accuracy" fix from the
   weighting caveat — inverted into earned trust, now justified by data.
4. **The sharp edge this data exposed:** WWV's marker fixes are ±30 s
   ambiguous (phase within a minute, not which minute). A device seeded by a
   single broken station (a WWOZ, 12 min off) would get its *phase* beautifully
   corrected and stay minutes wrong. The **minute must come from RDS consensus
   or the operator** — a lone unverified CT station may seed only with wide
   uncertainty and must never be minute-corroborated by WWV alone.
5. **Propagation and antenna were already architected for** on the HF side —
   band stepping 5/10/15 MHz with per-band success logging and learned
   preference resets naturally when conditions change; a better antenna just
   makes bands start succeeding. On FM, antenna/location changes are absorbed
   by rescanning. Time-of-day effects are why listen windows step bands at all.

Implementation lands after the current n≥2 survey run confirms offset
*stability* (the assumption behind earned trust).

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

