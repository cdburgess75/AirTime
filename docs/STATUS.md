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

> **Owner decision (2026-07-25): the Milestone 0 hardware gate is dropped for
> host-side development.** Because of the bricking risk, we build and unit-test
> the platform-independent core on the laptop first (no device touched). The M0
> safety steps below remain a **pre-flash** gate: nothing gets flashed to the
> ATS Mini until they're done — but they no longer block writing/testing core
> logic. See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the hardware seam.

---

## ⛔ Milestone 0 — Safety net & hardware verification (now a PRE-FLASH gate)
**Downgraded from "no code before green" to "no *flashing* before green" (owner decision above). All steps still need the physical device and are owner‑run.**

📖 **Full step-by-step runbook: [`MILESTONE0.md`](MILESTONE0.md)** — copy-pasteable commands,
the recovery drill, the IO11 beat test, a results table to fill in, and troubleshooting.

- [ ] Identify chip + partition layout (does anything live above 0x200000?)
- [ ] Back up stock firmware — **full 16 MB** (see amendment below), checksum it, verify it
- [ ] **Recovery drill** — rehearse forced download mode, then erase and restore on purpose
- [ ] Build **stock** `ats-mini` (correct PSRAM variant), flash, confirm normal radio operation
- [ ] IO11 verification — HJBerndt binary, 9999.000 kHz USB beat, backlight flicker = tap confirmed
      - [ ] If no flicker at any volume → original‑V4 pads → one jumper wire (amp pin 8 → IO11), retest
- [ ] Record outcomes in the [`MILESTONE0.md §7`](MILESTONE0.md#7-record-the-results) table and commit

> **Green when:** the recovery drill passed *and* the IO11 row is resolved (routed, or
> jumpered and then confirmed).
>
> Claude can help with: subtree setup, PlatformIO config, exact esptool commands, and
> documenting results. The flashing/probing itself is yours.

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
- [ ] Leading‑edge timestamp (`esp_timer`, µs)
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
[`hal.h`](../lib/airtime_core/hal.h) and *faked* in [`fakes.h`](../test/fakes.h),
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
all of Milestone 5 field acceptance.

**Blocked on the fork** — the device PlatformIO env, which needs the `ats-mini`
fork decision below.

## Setup decisions

1. **How the `ats-mini` base lives in git → `git subtree` at `firmware/ats-mini/`.**
   Pulled directly from upstream `esp32-si4732/ats-mini` (no GitHub fork needed unless we
   later contribute back). Rationale: the adapters must *edit* upstream files, so local
   modifications are unavoidable either way. A submodule would split glue code from the
   core across two repos and adds a clone-time failure mode (every remote session starts
   from a fresh clone; a missed `--recursive` yields a silently empty directory).
   Vendoring is simple but strands us against an active upstream. Subtree gives one repo,
   one clone, *and* a real merge path (`git subtree pull`). One line added to the vendored
   `platformio.ini` (`lib_extra_dirs = ../../lib`) picks up `airtime_core` unchanged.
   Commands in [`MILESTONE0.md §5a`](MILESTONE0.md#5a-bring-the-firmware-base-into-the-repo).

2. **Stock‑firmware backup → in‑repo if this repo is private; checksum-only if public.**
   A write-once 2 MB blob is the benign case for git, and the file's whole value is being
   *available* during a failure — in-repo means offsite, versioned, and auto-cloned into
   every future session. **Caveat:** it is AMNVOLT's copyrighted binary, so if the repo is
   public, keep the images out of git (local + cloud backup) and commit only the SHA-256.
   `SHA256SUMS` goes in the repo either way. No Git LFS — 2 MB doesn't warrant it.

   **Amendment to PLAN.md §7:** back up the **full 16 MB**, not `0x0`+`0x200000`. This is a
   16 MB part (N16R8); if stock firmware keeps SPIFFS/NVS/calibration data above 2 MB, a
   2 MB image would not fully restore it. Costs ~3 extra minutes. The 2 MB app-region
   image is still taken as a fast-restore option.
