# AirTime

**A multi-source, resilient over-the-air time reference for FT8/JS8 digital modes.**

AirTime runs entirely on an *unmodified* [AMNVOLT ATS Mini V4](docs/PLAN.md#2-target-hardware-owned-verified) pocket receiver. When internet and GPS are gone, time still arrives over the air — **FM RDS** and **WWV** — and AirTime arbitrates those sources into a drift-disciplined internal clock, then serves it to your laptop as **NTP**.

> Status: **the whole device runs — in simulation.** Every piece of AirTime's logic is written and host-tested (`make test` → **81 tests, 2151 checks passing**), and a simulated ATS Mini drives the real application end to end: it cold-starts from RDS, outvotes a station transmitting wrong time, lets WWV refine the fix, learns its crystal, and serves accurate stratum-1 NTP — with WiFi and the ADC never live at the same time. What remains is the thin firmware adapters and the one calibration constant only hardware can supply. Nothing is flashed yet: the Milestone 0 safety steps remain a **pre-flash** gate. See [`docs/STATUS.md`](docs/STATUS.md).

---

## Why

FT8/JS8 decoding needs the operating computer's clock within ~1 s of UTC (under 200 ms is effectively perfect). In the field, or after a hurricane, there is no internet NTP — and GPS is a single point of failure (jamming, spoofing, or simply no module on hand). Every commercial "field time server" is GPS‑only, and none of them arbitrate multiple over‑the‑air sources with honest uncertainty reporting. AirTime is that missing device.

## How it works

Three time sources feed one arbiter:

| Tier | Source | Provides | Notes |
|---|---|---|---|
| 1 | **FM RDS** clock‑time | Full date + time, ±hundreds of ms | Multi‑station **voting** is mandatory — many stations send no CT, some send wrong CT |
| 2 | **WWV** minute marker | Sub‑100 ms phase alignment, no date | 800 ms of 1000 Hz at the top of each minute on 2.5/5/10/15/20/25 MHz |
| 3 | **Manual** entry | Operator‑set time | Always available fallback |

The arbiter is the heart of the project. Its rules (full detail in [`docs/PLAN.md §4`](docs/PLAN.md#4-time-sources-and-arbiter)):

1. **The internal clock is the clock.** Sources never step it — they steer rate and phase.
2. Corrections **< 500 ms**: accepted from any single credible source; slewed smoothly, never stepped.
3. Corrections **≥ 500 ms**: require two independent sources agreeing, or explicit operator confirmation. (Defends against bad RDS CT and spoofing.)
4. **The crystal is learned.** Rate error is tracked across syncs and stored in NVS, so hourly syncing is sufficient.
5. **Uncertainty is first‑class state**, displayed always, and served time is flagged unsynchronized once the estimate crosses a threshold.

```
14:22:07 UTC
±60 ms · RDS+WWV · sync 3h ago · NTP: 2 clients
```

## Hardware

**AMNVOLT ATS Mini V4** — genuine, case‑branded (sub‑revision V4 or V4a).

- ESP32‑S3‑WROOM‑1‑N16R8 (dual core, WiFi, 16 MB flash, 8 MB PSRAM)
- SI4732‑A10 DSP receiver: 150 kHz–30 MHz AM/SSB, 64–108 MHz FM **with hardware RDS decode**
- 320×170 TFT, single push‑encoder, LiPo, SMA antenna port, USB‑C

Two hardware facts shape the whole design:

- **WWV audio is sampled on GPIO IO11 (ADC2_CH0)** via the factory amplifier‑output routing. *This is a standing assumption verified in Milestone 0; if it fails, the fix is a single jumper wire and nothing else changes.*
- **ADC2 cannot be read while WiFi is active** (an ESP32 silicon limit). So **all WWV listening happens with WiFi down**; FM/RDS reception is unaffected and runs regardless.

## What v1 is deliberately *not*

No GPS module · no external RTC (DS3231) · no WWV date/timecode decode (phase only) · no soldering (contingent on the IO11 assumption) · not a general receiver UI — it is a clock appliance. These are settled decisions; see [`docs/PLAN.md §3`](docs/PLAN.md#3-what-v1-is-not-settled-decisions).

## Roadmap

| Milestone | Deliverable | State |
|---|---|---|
| **0 — Safety net + HW verify** | Stock firmware backed up, recovery drill done, IO11 tap confirmed | ⛔ **Pre-flash gate — hardware, owner‑run.** Runbook ready: [`docs/MILESTONE0.md`](docs/MILESTONE0.md) |
| **1 — RDS clock** | Self‑setting clock from broadcast FM | 🟡 Decode + voting done (host) |
| **2 — Serve** | Laptop runs FT8 synced to the radio, no internet | 🟡 SNTP + client counting + unsynced flagging done (host) |
| **3 — WWV phase lock** | Clock disciplines itself from HF with FM absent | 🟡 Goertzel, marker gate, band stepping done (host) |
| **4 — Arbiter + confidence** | Full AirTime runtime behavior | 🟡 Arbiter, clock, drift, uncertainty, scheduler done (host) |
| **5 — Field acceptance** | Cold start → laptop synced → WSJT‑X DT ≈ 0 all evening | ⬜ Not started |

Detailed milestone contents live in [`docs/PLAN.md §7`](docs/PLAN.md#7-milestones) and are tracked in [`docs/STATUS.md`](docs/STATUS.md).

## Build

- **Base:** a fork of [`esp32-si4732/ats-mini`](https://github.com/esp32-si4732/ats-mini) — active community firmware for this exact hardware, WiFi/web stack included.
- **Toolchain:** [PlatformIO](https://platformio.org/), driven from Claude Code on Linux.
- **PSRAM variant matters:** the fork ships OSPI and QSPI builds; the correct one shows nonzero PSRAM in Settings→About. Determine which this V4 needs before the first flash.

## Using AirTime as your time source

WSJT‑X and JS8Call have **no NTP client** — they read the OS clock. So you point your **operating system** at the radio, and the OS keeps FT8's clock honest.

Once AirTime is serving (SoftAP up, NTP responding at the radio's IP, e.g. `192.168.4.1`):

- **Windows** — [Meinberg NTP](https://www.meinbergglobal.com/english/sw/ntp.htm) pointed at the radio's IP, or `w32tm`:
  ```powershell
  w32tm /config /manualpeerlist:"192.168.4.1" /syncfromflags:manual /update
  w32tm /resync
  ```
- **Linux** — `chrony` (or `ntpd`):
  ```
  # /etc/chrony/chrony.conf
  server 192.168.4.1 iburst prefer
  ```
  then `sudo systemctl restart chrony && chronyc sources`

Confirm success in WSJT‑X: the **DT column** should cluster near zero.

## Repository layout

```
AirTime/
├── README.md              You are here
├── Makefile               Host build + unit tests (make test)
├── platformio.ini         PlatformIO envs (native now; device env in M1)
├── lib/
│   └── airtime_core/         Platform-independent core (no Arduino/ESP-IDF)
│       ├── goertzel.*        1000 Hz WWV tone detector
│       ├── rds_ct.*          RDS group 4A clock-time decode
│       ├── station_vote.*    Multi-station CT voting
│       ├── wwv_marker.*      WWV minute-marker gate (duration + noise floor)
│       ├── disciplined_clock.*  Slew/rate-steered internal clock
│       ├── drift.*           Crystal drift learning
│       ├── arbiter.*         Multi-source arbiter (the heart, §4)
│       ├── sntp.*            NTP server packets + client counting
│       ├── scheduler.*       Acquisition, listen windows, band stepping
│       ├── hal.h             The hardware seam (interfaces)
│       ├── display.*         The §5 display lines
│       └── app.*             AirTimeApp — wires it all to the seam
├── test/                     Unit tests (81 cases) + fakes.h, a simulated ATS Mini
├── tools/
│   └── inspect_flash.py   Validate / compare ESP32 flash images (Milestone 0)
└── docs/
    ├── PLAN.md            The canonical v1 specification and build plan
    ├── ARCHITECTURE.md    The core ↔ hardware seam
    ├── MILESTONE0.md      Backup / recovery-drill / IO11 runbook (run before flashing)
    └── STATUS.md          Live milestone / task tracker
```

The `ats-mini` firmware base is brought in as a **git subtree** at `firmware/ats-mini/`
during Milestone 0 (§5a of the runbook); it consumes `lib/airtime_core` unchanged.

Device firmware (the thin hardware adapters that feed the core) is added starting
in Milestone 1; it consumes `lib/airtime_core` unchanged and is only *flashed*
once the Milestone 0 safety steps are done.

## References

- Fork base — https://github.com/esp32-si4732/ats-mini · docs https://esp32-si4732.github.io/ats-mini/
- HJBerndt firmware & IO11/Goertzel documentation — http://www.hjberndt.de/dvb/pocketSI4735DualCoreDecoder.html
- V4 reverse engineering (Peter Neufeld, V4XXL) — https://peterneufeld.wordpress.com/2025/12/05/v4xxl-mini-radio-to-the-max/
- WWV/WWVH signal format (NIST) — https://www.nist.gov/pml/time-and-frequency-division/time-services/

Full reference list in [`docs/PLAN.md §8`](docs/PLAN.md#8-references).
