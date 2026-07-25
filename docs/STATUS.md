# AirTime — Status & Task Tracker

Live checklist for the build. Milestone contents come from [`PLAN.md §7`](PLAN.md#7-milestones).
Legend: ⬜ not started · 🟡 in progress · ✅ done · ⛔ blocked/gate

Last updated: batch 1 of the host-tested core is green — Goertzel, RDS 4A
decode, and multi-station voting (16 tests / 78 checks passing via `make test`).

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

## ⬜ Milestone 2 — Serve
**Deliverable: laptop runs FT8 synced to the radio, no internet.**

- [ ] SoftAP up
- [ ] NTP/SNTP responder
- [ ] Client counting
- [ ] Unsynchronized flagging (stratum / leap‑indicator semantics)
- [ ] OS client setup documented (done in README; revisit after real testing)

## 🟡 Milestone 3 — WWV phase lock
**Deliverable: clock disciplines itself from HF with FM absent.**

- [x] Goertzel 1000 Hz detector — `goertzel` ✅ host-tested (core‑2/IO11 wiring is the `Sampler` adapter)
- [ ] Minute‑marker detection: duration gate (700–900 ms) + noise‑floor threshold *(batch 2: `wwv_marker`)*
- [ ] Leading‑edge timestamp (`esp_timer`, µs)
- [ ] WiFi‑down listen windows (NTP clients coast through)
- [ ] Band stepping 5/10/15 MHz with per‑band success + SNR logging
- [ ] Calibration constant (measure once; validate via WSJT‑X DT)

## ⬜ Milestone 4 — Arbiter + confidence
**Deliverable: the full AirTime runtime behavior of §5.**

- [ ] Slew/step rules (<500 ms slew; ≥500 ms needs 2 sources or operator confirm)
- [ ] Two‑source requirement for large corrections
- [ ] Drift learning → NVS
- [ ] Uncertainty computation + always‑on display
- [ ] Boot‑time parallel acquisition (RDS vote + WWV band‑step, WiFi down)
- [ ] Hourly listen scheduler

## ⬜ Milestone 5 — Field acceptance
**Deliverable: cold start → laptop synced → WSJT‑X DT ≈ 0 across an evening.**

- [ ] Battery‑only, external antenna, no infrastructure cold‑start test
- [ ] Evening of decodes: DT column clusters near 0
- [ ] Multi‑day soak validates drift learning

---

## Open setup questions (need an owner decision)

1. **How should the `ats-mini` fork live in git?** Options: (a) fork on GitHub and add it as a
   git **submodule** here; (b) **vendor** a snapshot of the sources into this repo; (c) develop
   AirTime as commits **on the fork itself** and keep this repo for spec/docs only. Affects how
   upstream updates are pulled in.
2. **Where does the stock‑firmware backup `.bin` go?** In‑repo (simple, but a ~2 MB binary blob)
   or stored outside git with a checksum recorded here.
