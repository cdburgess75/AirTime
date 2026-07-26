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

- [ ] Survey receivable FM stations; note which send RDS CT *(hardware)*
- [x] RDS CT‑group (group 4A) decode — `rds_ct` ✅ host-tested
- [x] Multi‑station **voting** logic — `station_vote` ✅ host-tested (scan is hardware)
- [ ] Minute‑boundary set *(needs disciplined clock — batch 2)*
- [ ] Timezone config
- [ ] Persist last‑known date/time to NVS *(needs `TimeStore` adapter)*

## 🟡 Milestone 2 — Serve
**Deliverable: laptop runs FT8 synced to the radio, no internet.**

- [ ] SoftAP up *(WiFi adapter — device)*
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

| Interface | Device implementation |
|---|---|
| `IMonotonicClock` | `esp_timer_get_time()` |
| `IRdsSource` | SI4732 RDS group registers |
| `IWwvSampler` | ADC2_CH0 on IO11 + our `Goertzel`, pinned to core 2 |
| `IWiFiControl` | SoftAP up/down |
| `ITimeStore` | NVS |
| *(not an interface)* | UDP/123 socket → `AirTimeApp::handleNtpRequest` |
| *(not an interface)* | TFT + encoder → `AirTimeApp::displayState` / operator calls |

**Genuinely hardware-dependent** — the calibration constant (§4: SI4732 DSP group
delay + amp + ADC latency, est. 10–40 ms), the local FM station survey (M1), and
all of Milestone 5 field acceptance. Milestone 0 measured two useful priors for this
work: the receiver reads **~250 Hz low at 10 MHz (~25 ppm)**, and the receive chain
carries a steady **~2750 Hz noise component at rms ≈ 37 ADC counts** with the volume at
zero — the floor `wwv_marker` thresholds against.

**Firmware base** — `esp32-si4732/ats-mini` is vendored at `firmware/ats-mini/` as a
git subtree. It builds with **Arduino CLI**, not PlatformIO as PLAN.md §6 assumed; the
repo's own `platformio.ini` covers the host test build only. Integrating `airtime_core`
into an Arduino sketch build is a Milestone 1 task.

## Open design question: uncertainty-weighted steering

**`TimeFix.uncertainty_us` is carried but never used to weight a correction.** The
arbiter applies every accepted fix in full, so **the source that reports more often
wins, regardless of which is more precise** — the opposite of what §4's tiering intends.

Demonstrated in simulation: one RDS station biased 220 ms late, submitting every ~75 s,
against WWV landing once an hour. WWV is accepted and credited, but the clock settles at
RDS's 220 ms bias — even though RDS declares ±250 ms and WWV ±30 ms, and §4 puts WWV
above RDS *precisely* for phase accuracy.

The principled fix is a variance-weighted gain, applying a fraction of each correction:

    gain = our_variance / (our_variance + source_variance)

With our uncertainty at 30 ms and RDS claiming 250 ms, that gain is ~0.014 — RDS barely
moves a WWV-disciplined clock, while still dominating when we are badly out. It also
makes §4 rule 5's "uncertainty is first-class state" load-bearing rather than decorative.

This changes the arbiter — "the heart" — and PLAN.md does not specify it, so it is
flagged rather than assumed. `app_wwv_refines_phase` asserts today's real behaviour and
carries a pointer here.

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

