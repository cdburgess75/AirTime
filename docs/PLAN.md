# AirTime

A multi-source resilient time reference for FT8/JS8 digital modes. Runs entirely on an unmodified AMNVOLT ATS Mini V4 pocket receiver. When internet and GPS are gone, time still arrives over the air — FM RDS and WWV — and AirTime serves it to your laptop as NTP.

**This document is the v1 specification and build plan.** It assumes a fresh Claude Code session with a GitHub repository. All product decisions below are settled; do not reopen them without the owner.

---

## 1. The problem

FT8/JS8 decoding requires the operating computer's clock to be within ~1 s of UTC (under 200 ms is effectively perfect). In the field or after a hurricane there is no internet NTP, and GPS is a single point of failure (jamming, spoofing, or simply no module on hand). Every commercial "field time server" is GPS-only. Nothing on the market arbitrates multiple over-the-air time sources with honest uncertainty reporting. AirTime is that device.

## 2. Target hardware (owned, verified)

**AMNVOLT ATS Mini V4** (genuine, case-branded; sub-revision V4 or V4a — theremin test showed SSB pitch-pull, ruling out V4b).

- ESP32-S3-WROOM-1-N16R8 (dual core, WiFi, 16 MB flash, 8 MB PSRAM)
- SI4732-A10 DSP receiver: 150 kHz–30 MHz AM/SSB, 64–108 MHz FM **with hardware RDS decode**
- 320×170 TFT, single push-encoder, LiPo, SMA antenna port, USB-C
- **Factory-routed audio path from the amplifier output to ESP32 GPIO IO11 (ADC2_CH0)** on V4a/V4b boards; original V4 has solder pads only

### Standing assumption (VERIFY FIRST — see Milestone 0)

> **ASSUMED: IO11 carries speaker audio on this unit.** Unverified. If the verification test fails, the fix is a single jumper wire (amp IC pin 8 → IO11) and nothing else in this plan changes.

### Hard silicon constraint

**ADC2 (which includes IO11) cannot be read while WiFi is active.** This is an ESP32 limitation, not a firmware choice. All WWV listening happens with WiFi down. FM/RDS reception does NOT touch ADC2 and runs regardless of WiFi state.

## 3. What v1 is NOT (settled decisions)

- **No GPS module.** Deferred to a possible v3 as an optional Tier 0. v1 is deliberately zero-hardware.
- **No DS3231 / external RTC.** Holdover rides on the ESP32 crystal (±20 ppm, ~1.7 s/day) with learned drift correction; RDS re-sync covers the gap. Revisit only if field experience shows holdover gaps bite.
- **No WWV timecode (date) decoding.** The BCD timecode rides a 100 Hz subcarrier below the SI473x audio passband. WWV provides *phase only* (second/minute alignment). Date and minute number come from RDS, warm-boot RTC memory, or manual entry.
- **No soldering** (contingent on the IO11 assumption).
- **Not a general receiver UI.** The device is a clock appliance; radio functions serve the clock.

## 4. Time sources and arbiter

```
Tier 1  FM RDS clock-time  → full date + time, ±hundreds of ms. Multi-station VOTING mandatory
                             (many US stations send no CT group; some send wrong/offset CT).
Tier 2  WWV minute marker  → sub-100 ms phase alignment, no date. 800 ms of 1000 Hz at
                             the top of each minute on 2.5/5/10/15/20/25 MHz.
Tier 3  Manual entry       → encoder/NumPad-style time set. Always available.
          ↓
   Arbiter + drift-learned internal clock → display + NTP server
```

### Arbiter rules (the heart of the project)

1. **The internal clock is the clock.** Sources never step it directly; they steer rate and phase.
2. Corrections **< 500 ms**: accept from any single credible source; slew smoothly, never step.
3. Corrections **≥ 500 ms**: require two independent sources agreeing within tolerance, OR explicit operator confirmation on the encoder. (Defends against bad RDS CT and any future spoofed source.)
4. **Learn the crystal.** Track rate error across successive syncs; store correction in NVS. A characterized ±20 ppm crystal behaves like a much better one; this is what makes hourly (not constant) syncing sufficient.
5. **Uncertainty is first-class state**: `±(time since last sync × learned drift rate + source uncertainty)`. Displayed always. Served time is marked unsynchronized (NTP leap-indicator/stratum semantics) when the estimate exceeds a threshold.

### WWV detection specifics

- Goertzel filter at **1000 Hz** on **core 2**, sampling IO11 ADC (or later I²S). Proven technique on this exact hardware (HJBerndt firmware — see §8).
- **Duration-gate the minute marker at 700–900 ms.** WWV also carries continuous 500/600 Hz standard tones and a 440 Hz hourly tone; the gate plus Goertzel selectivity rejects them. The 5 ms seconds ticks (5 cycles at 1 kHz) are likely undetectable via ADC — treat as bonus, never required.
- Timestamp the **leading edge** (µs, `esp_timer`); threshold against a running noise-floor estimate so AGC pumping doesn't shift the detected edge.
- Ticks are omitted at seconds 29 and 59 — free sanity check if tick detection ever works.
- WWVH uses 1200 Hz markers, so a 1000 Hz detector inherently prefers Fort Collins (correct for a Louisiana station, ~5 ms propagation — ignorable).
- **Band stepping:** cycle 5/10/15 MHz (add 2.5/20 as options), dwell ≥ 2 min each. Log per-band success + SNR to learn local propagation (expect 5 MHz at night, 10/15 by day).
- **Calibration constant:** one fixed offset lumps SI4732 DSP group delay (unpublished, est. 10–40 ms) + amp + ADC latency. Measure once against any decent reference (even a phone NTP clock); final acceptance is the WSJT-X **DT column** clustering near zero.

## 5. Runtime behavior (settled)

**Serve-mostly with boot-time acquisition:**

- **Every power-on:** WiFi stays DOWN. Parallel hunt — RDS scan/vote AND WWV band-step. First credible fix seeds the clock; a second within the window raises confidence. On fix OR timeout (default 5 min, configurable), bring up SoftAP + NTP and serve, displaying honestly: `synced ±80ms · WWV+RDS` vs `UNSYNCED — last-known + drift`.
- **Steady state:** NTP served continuously. Brief scheduled WWV listen window each hour (WiFi torn down for the window; NTP clients coast through it). RDS re-sync runs continuously in the background — no WiFi conflict.
- **Operator override:** manual "listen now" and "serve now" actions on the encoder menu.

**Display (concept):**
```
14:22:07 UTC
±60 ms · RDS+WWV · sync 3h ago · NTP: 2 clients
```
UTC primary (FT8 wants UTC). Local time optional via config.

**NTP consumption note:** WSJT-X/JS8Call have no NTP client — they read the OS clock. The laptop's OS points at the radio's IP (Meinberg NTP or `w32tm` on Windows; chrony/ntpd on Linux). Document this in the README.

## 6. Toolchain and repo (settled)

- **Base:** fork of `esp32-si4732/ats-mini` (github.com/esp32-si4732/ats-mini) — active community firmware for this exact hardware, WiFi/web stack included.
- **Build:** PlatformIO, driven from Claude Code on Linux.
- **PSRAM variant matters:** the fork ships OSPI and QSPI builds; correct one shows nonzero PSRAM in Settings→About. Determine which this V4 needs before first flash.
- HJBerndt's firmware is **closed-source** (binary only). It is prior art and a hardware-verification tool, not donor code. The Goertzel/dual-core technique is reimplemented from his public documentation (a Goertzel filter is ~30 lines).

## 7. Milestones

### Milestone 0 — Safety net and hardware verification (NO AirTime code before this is green)
1. **Back up stock firmware**: esptool/Flash Download Tool, read `0x0` size `0x200000`, commit the .bin to the repo (or store safely).
2. **Recovery drill** (owner has bricked a device before — this milestone exists for that reason): practice forced download mode (BOOT held at power-on, or DTR/RTS toggle over serial), Erase All, restore the backup, confirm boot. The ESP32-S3 ROM loader is mask ROM and cannot be destroyed by flashing; internalize that.
3. Build **stock** `ats-mini` from the fork, unmodified, correct PSRAM variant; flash; confirm normal radio operation.
4. **IO11 verification (retires the standing assumption):** flash HJBerndt binary; tune 9999.000 kHz USB (WWV 10 MHz carrier → continuous 1 kHz beat; evening best, or 4999.000 vs WWV 5 MHz); volume ~35; Decoder→Tune/BL; backlight flickering with the tone = tap confirmed. No flicker at any volume = original-V4 pads → one jumper wire, then retest. Reflash stock/fork after.

### Milestone 1 — RDS clock
CT-group decode, multi-station scan + voting, minute-boundary set, timezone config, persistence of last-known date/time to NVS. **Deliverable: self-setting clock from broadcast FM.**

### Milestone 2 — Serve
SoftAP + NTP server (SNTP responder is sufficient), client counting, unsynchronized flagging. OS-level client setup documented. **Deliverable: laptop runs FT8 synced to the radio, no internet.**

### Milestone 3 — WWV phase lock
Core-2 Goertzel on IO11, minute-marker detection with duration gate + noise-floor threshold, WiFi-down listen windows, band stepping with success logging, calibration constant. **Deliverable: clock disciplines itself from HF with FM absent.**

### Milestone 4 — Arbiter + confidence
Slew/step rules, two-source requirement for large corrections, drift learning, uncertainty display, boot-time acquisition sequence, hourly listen scheduler. **Deliverable: the full AirTime behavior of §5.**

### Milestone 5 — Field acceptance
Battery-only, external antenna, no infrastructure: power on cold → acquires → laptop syncs → WSJT-X DT column clusters near 0 across an evening of decodes. Multi-day soak for drift-learning validation.

## 8. References

- Fork base: https://github.com/esp32-si4732/ats-mini · docs: https://esp32-si4732.github.io/ats-mini/
- HJBerndt firmware & IO11/Goertzel documentation: http://www.hjberndt.de/dvb/pocketSI4735DualCoreDecoder.html
- V4 reverse engineering & fixes (Peter Neufeld, V4XXL): https://peterneufeld.wordpress.com/2025/12/05/v4xxl-mini-radio-to-the-max/
- V4 RF issues catalog: github.com/vegos/RadioExperiments (MiniATS/Amnvolt_V4_mods)
- Vendor (genuine-unit list, flashing notes): https://atsmini.github.io/
- WWV/WWVH signal format: https://www.nist.gov/pml/time-and-frequency-division/time-services/
- Original V1 schematic (closest public reference): https://xtronic.org/circuit/rf/radio-receiver/esp32-s3-si4732-pocket-multiband-receiver/

## 9. Risk register

| Risk | Standing answer |
|---|---|
| IO11 not connected (original V4) | One jumper wire, amp pin 8 → IO11; everything else unchanged |
| ADC2 dead under WiFi | Designed in: listen windows are WiFi-down by definition |
| No HF propagation for days | Learned-drift holdover + RDS keeps FT8 tolerance |
| RDS CT absent/wrong locally | Mandatory multi-station voting; survey receivable stations in M1 |
| ESP32/display RFI desensing HF | SMA + external antenna (owner's doublet), lower backlight in listen windows |
| Crystal drift worse than spec | Drift learning compensates; DS3231 remains the v2 escape hatch |
| Flash mishap | Milestone 0 recovery drill; mask-ROM loader is indestructible |
| Theremin effect (V4/V4a) | SSB-only quirk; irrelevant to FM RDS and 800 ms tone detection |
