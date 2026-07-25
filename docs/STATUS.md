# AirTime — Status & Task Tracker

Live checklist for the build. Milestone contents come from [`PLAN.md §7`](PLAN.md#7-milestones).
Legend: ⬜ not started · 🟡 in progress · ✅ done · ⛔ blocked/gate

Last updated: batch 3 adds the SNTP responder and the acquisition/listen
scheduler. The core is now feature-complete against the spec's *logic*:
**58 tests / 1903 checks** passing via `make test`, including a closed-loop
simulation where a 25 ppm-slow crystal is learned (offset → < 20 ms/hour) and an
8-hour sweep proving WiFi and ADC2 are never enabled together (§2).

> **Owner decision (2026-07-25): the Milestone 0 hardware gate is dropped for
> host-side development.** Because of the bricking risk, we build and unit-test
> the platform-independent core on the laptop first (no device touched). The M0
> safety steps below remain a **pre-flash** gate: nothing gets flashed to the
> ATS Mini until they're done — but they no longer block writing/testing core
> logic. See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the hardware seam.

---

## ⛔ Milestone 0 — Safety net & hardware verification (now a PRE-FLASH gate)
**Downgraded from "no code before green" to "no *flashing* before green" (owner decision above). All steps still need the physical device and are owner‑run.**

- [ ] Back up stock firmware (read `0x0`, size `0x200000`); commit the `.bin` (or store safely)
- [ ] Recovery drill — forced download mode, Erase All, restore backup, confirm boot
- [ ] Build **stock** `ats-mini` from the fork (correct PSRAM variant), flash, confirm normal radio operation
- [ ] IO11 verification — HJBerndt binary, 9999.000 kHz USB beat, backlight flicker = tap confirmed
      - [ ] If no flicker at any volume → original‑V4 pads → one jumper wire (amp pin 8 → IO11), retest
- [ ] Record outcome: which V4 sub‑revision, PSRAM variant (OSPI/QSPI), IO11 confirmed vs jumpered

> Claude can help with: fork setup, PlatformIO config, the exact esptool/backup commands, and
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

**Firmware adapters** (thin shims; see [`ARCHITECTURE.md`](ARCHITECTURE.md)) —
`RdsSource` (SI4732 RDS registers), `Sampler` (ADC2_CH0 on IO11, core 2),
`MonotonicClock` (`esp_timer`), `DriftStore`/`TimeStore` (NVS), `NtpResponder`
(lwIP UDP/123), `WiFiControl` (SoftAP up/down), `Ui` (TFT + encoder).

**Genuinely hardware-dependent** — the calibration constant (§4: SI4732 DSP group
delay + amp + ADC latency, est. 10–40 ms), the local FM station survey (M1), and
all of Milestone 5 field acceptance.

**Blocked on the fork** — the device PlatformIO env, which needs the `ats-mini`
fork decision below.

## Open setup questions (need an owner decision)

1. **How should the `ats-mini` fork live in git?** Options: (a) fork on GitHub and add it as a
   git **submodule** here; (b) **vendor** a snapshot of the sources into this repo; (c) develop
   AirTime as commits **on the fork itself** and keep this repo for spec/docs only. Affects how
   upstream updates are pulled in.
2. **Where does the stock‑firmware backup `.bin` go?** In‑repo (simple, but a ~2 MB binary blob)
   or stored outside git with a checksum recorded here.
