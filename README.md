# AirTime

**A multi-source, resilient over-the-air time reference for FT8/JS8 digital modes.**

AirTime runs entirely on an *unmodified* [AMNVOLT ATS Mini V4](docs/PLAN.md#2-target-hardware-owned-verified) pocket receiver. When internet and GPS are gone, time still arrives over the air — **FM RDS** and **WWV** — and AirTime arbitrates those sources into a drift-disciplined internal clock, then serves it to your laptop as **NTP**.

> Status: **It works on the radio.** `sntp` against the device returns
> **−0.005 s ± 0.071** with no internet and no GPS: RDS seeds the date from
> broadcast FM, a WWV minute marker on 15 MHz pulls the phase onto the second,
> and the laptop takes its time from the radio's own access point. Milestones
> 0–4 are verified on hardware; what remains is an evening of FT8 decodes
> (Milestone 5). `make test` → **174 tests, 6359 checks passing**. The story of
> how it got there — including the bugs that each made the device
> *confidently wrong* — is in [`docs/STATUS.md`](docs/STATUS.md).

```
                                    AirTime          [batt][wifi]

                       18:31:26  CDT
                       23:31:26  UTC

                 +/-31 ms   RDS+WWV   sync 2m ago

 FM 89.9 MHz  RDS                          NTP: 1 client
 ┌────────────────────────────────────────────────────────┐
 │ FT8 15s ███████████████░░░░░░░░░░░░░░░░░░░░░░  T-8.43s │
 └────────────────────────────────────────────────────────┘
```

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

## What it does besides keep time

The clock came first and everything here is downstream of it — each of these is
something a receiver can only do once it knows UTC to milliseconds, or something
the operator wanted while the radio was already on the bench.

- **FT8/JS8 cycle instrument.** Turn the encoder on the clock face and a bar
  fills across the current transmit slot with a millisecond countdown to the
  next one — FT8, FT4, **FT2** (3.75 s), JS8 (Normal/Fast/Turbo/Slow), JT65, WSPR. The fill colour
  follows **slot parity**, so FT8's transmit/receive alternation is visible at a
  glance. It doubles as a check on the computer being disciplined: if WSJT‑X
  starts transmitting at a visibly different moment than the bar rolls over, the
  laptop's clock is wrong, caught by eye from across the desk.
- **A phone app, served by the radio.** Join the radio's access point, open
  `192.168.4.1`, Add to Home Screen. Full‑screen clock, live cycle bar, and the
  diagnostics — rendering local time from *the radio's* UTC, never the phone's.
- **Net schedules.** ~15 HF nets with their published times, so the screen can
  answer "is it on **now**" rather than "what frequency is it on". Selecting one
  tunes the radio to it.
- **The EiBi shortwave schedule, embedded.** 8,142 broadcast entries compiled
  into the firmware, because this device's network is a bare access point with
  no route to the internet — the schedule cannot be downloaded, so it ships
  inside the image. Tune around in radio mode and stations identify themselves.
- **CW copy.** A Morse decoder on the audio tap, with a tuning bar, because CW
  is tuned by ear and the decoder only hears one 200 Hz bin.
- **Waterfall.** The SSB passband as spectrum plus history, 64 bins across
  150–3300 Hz.
- **Radio mode.** The full stock ats‑mini receiver UI is still there. Ask for it
  and AirTime lets go of the dial entirely.

CW copy and the waterfall take the audio tap, which means the access point goes
down while they are on screen — the same ADC2/WiFi constraint that shapes the
whole design, stated on the screen rather than left to be discovered.

## Hardware

**AMNVOLT ATS Mini V4** — genuine, case‑branded. Determined to be a **V4a**: the factory audio tap to IO11 is present (measured, Milestone 0 §6).

- ESP32‑S3‑WROOM‑1‑N16R8 (dual core, WiFi, 16 MB flash, 8 MB PSRAM)
- SI4732‑A10 DSP receiver: 150 kHz–30 MHz AM/SSB, 64–108 MHz FM **with hardware RDS decode**
- 320×170 TFT, single push‑encoder, LiPo, SMA antenna port, USB‑C

Two hardware facts shape the whole design:

- **WWV audio is sampled on GPIO IO11 (ADC2_CH0)** via the factory amplifier‑output routing. ✅ **Measured and confirmed on the owner's unit (2026-07-25)** — no jumper wire needed. This unit is a V4a.
- **ADC2 cannot be read while WiFi is active** (an ESP32 silicon limit). So **all WWV listening happens with WiFi down**; FM/RDS reception is unaffected and runs regardless.

There is also a **single tuner**. FM (for RDS) and AM (for WWV) are mutually
exclusive, and forgetting it is the most productive bug class this project has
had — see the note below.

## What v1 is deliberately *not*

No GPS module · no external RTC (DS3231) · no WWV date/timecode decode (phase only) · **no soldering — confirmed unnecessary** · not a general receiver UI — it is a clock appliance. These are settled decisions; see [`docs/PLAN.md §3`](docs/PLAN.md#3-what-v1-is-not-settled-decisions).

## Roadmap

| Milestone | Deliverable | State |
|---|---|---|
| **0 — Safety net + HW verify** | Stock firmware backed up, recovery drill done, IO11 tap confirmed | ✅ **COMPLETE** — drill passed, **IO11 tap confirmed**, no jumper needed: [`docs/MILESTONE0.md`](docs/MILESTONE0.md) |
| **1 — RDS clock** | Self‑setting clock from broadcast FM | ✅ **ON DEVICE** — cold start to sync in 30 s; dial surveyed, stations scored: [`docs/MILESTONE1.md`](docs/MILESTONE1.md) |
| **2 — Serve** | Laptop runs FT8 synced to the radio, no internet | ✅ **ON DEVICE** — laptop served over the radio's own AP |
| **3 — WWV phase lock** | Clock disciplines itself from HF with FM absent | ✅ **ON DEVICE** — 15 MHz minute marker detected and applied; `sntp` → **−0.005 s ± 0.071** |
| **4 — Arbiter + confidence** | Full AirTime runtime behavior | ✅ Slew/step, corroboration, per‑source drift, honest uncertainty — all exercised over the air |
| **5 — Field acceptance** | Cold start → laptop synced → WSJT‑X DT ≈ 0 all evening | 🟡 Client setup documented ([`docs/CLIENT_SETUP.md`](docs/CLIENT_SETUP.md)); runbook and analysis ready ([`docs/MILESTONE5.md`](docs/MILESTONE5.md)); evening of decodes pending |

Detailed milestone contents live in [`docs/PLAN.md §7`](docs/PLAN.md#7-milestones) and are tracked in [`docs/STATUS.md`](docs/STATUS.md).

## The bug class worth knowing about

Nearly every field failure in this project has been the same shape: **something
held a cached claim about the radio, and the radio had moved.**

- The test fakes modelled FM and AM as two independent radios. The device has
  one. Giving the fakes a single shared tuner surfaced two real bugs within
  minutes — including listen windows that were sampling FM program audio while
  the log confidently printed a WWV frequency.
- AirTime tuned the SI4732 directly and never updated stock's `currentMode`, so
  the rest of the firmware kept reasoning about the operator's last band. The
  S‑meter was drawn on the HF curve while the chip sat on FM — six bars from an
  ordinary station — and the AGC table applied to the front end belonged to a
  different mode entirely. That one presented as "it just searches, but the
  signal is strong", and the strong signal was the artifact.

The design response is in the code and in [`docs/STATUS.md`](docs/STATUS.md):
one function owns tuning, cached claims are voided when the dial moves, and the
status page prints the chip's own story next to ours so a disagreement is
visible instead of inferred.

## Build

- **Base:** a fork of [`esp32-si4732/ats-mini`](https://github.com/esp32-si4732/ats-mini) — active community firmware for this exact hardware, WiFi/web stack included.
- **Toolchain:** [Arduino CLI](https://arduino.github.io/arduino-cli/) for the device build; plain `g++`/`make` for the host core and its tests.
- **PSRAM variant:** upstream ships `esp32s3-ospi` and `esp32s3-qspi` profiles. This unit reports 8 MB PSRAM (the `R8`, octal), so **OSPI** — which is also upstream's default. Confirm via non-zero PSRAM in Settings→About.

```
make test                  # host core: 174 tests, 6359 checks
tools/build_fw.sh airtime  # the device build
tools/build_fw.sh stock    # upstream, unmodified — see below
```

**The AirTime build is additive.** Every change to upstream files is inside
`#ifdef AIRTIME`, and `tools/build_fw.sh stock` is expected to produce a
**byte‑identical** image to unmodified upstream. That number is checked on every
commit; when it moves, something leaked out of the guard and the diff is wrong
until it moves back.

Versions are `vYYYY.MM.DD.NNN` — the date, then the build number for that day.
`tools/bump_version.sh` advances it. The running version is on the **About**
screen (first item in the menu), in the phone app's header, and in the serial
banner. Telling two builds apart by comparing binary sizes has already produced
one false alarm in this project.

### Flashing without a toolchain

`tools/release.sh` produces one merged image — bootloader, partition table and
application in a single file at offset 0, which is what a web flasher expects
and which removes the commonest way a first flash goes wrong:

```
tools/release.sh                       # -> dist/airtime-<version>-airtime.bin
esptool.py --chip esp32s3 write_flash 0x0 dist/airtime-<version>-airtime.bin
```

The first boot after a flash that carries new schedule data spends a few seconds
parsing it into LittleFS, with a progress screen. Every boot after that is a
hash compare.

## Using AirTime as your time source

WSJT‑X and JS8Call have **no NTP client** — they read the OS clock. So you point your **operating system** at the radio, and the OS keeps FT8's clock honest.

**The short version, on any of the three platforms:** join the radio's WiFi, then

```
./tools/airtime-sync.sh            # sync once
sudo ./tools/airtime-sync.sh --install   # ...or track it automatically, forever
```

It refuses to sync from a radio that reports itself unsynchronised, and does
nothing at all when the radio is not there — which is what makes the installed
version safe to forget about. Full per-OS detail, including what the WSJT‑X
**DT** column is telling you, is in [`docs/CLIENT_SETUP.md`](docs/CLIENT_SETUP.md).

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
`tools/dt_report.py` will chew a WSJT‑X log and report the median for you.

### What the radio serves

| Path | |
|---|---|
| `udp/123` | NTP, with honest root dispersion — the uncertainty above, not a fiction |
| `/` | the phone app |
| `/status` | the full diagnostic table; the first place to look when something is wrong |
| `/api` | the same state as flat JSON |

The access point — and everything on it — goes down during each WWV listening
window, because ADC2 and the WiFi radio cannot both be live. That is the device
working correctly; it comes back by itself.

## Repository layout

```
AirTime/
├── README.md              You are here
├── Makefile               Host build + unit tests (make test)
├── platformio.ini         Host test env only (device build uses Arduino CLI)
├── lib/
│   └── airtime_core/         Platform-independent core (no Arduino/ESP-IDF)
│       ├── library.properties   Arduino library manifest
│       └── src/airtime/        headers are <airtime/…> to avoid collisions
│           ├── goertzel.*        1000 Hz WWV tone detector
│           ├── rds_ct.*          RDS group 4A clock-time decode
│           ├── station_vote.*    Multi-station CT voting
│           ├── station_bias.*    Per-station learned offset
│           ├── wwv_marker.*      WWV minute-marker gate (duration + noise floor)
│           ├── disciplined_clock.*  Slew/rate-steered internal clock
│           ├── drift.*           Crystal drift learning
│           ├── arbiter.*         Multi-source arbiter (the heart, §4)
│           ├── sntp.*            NTP server packets + client counting
│           ├── scheduler.*       Acquisition, listen windows, band stepping
│           ├── fm_survey.*       Self-survey of the local FM dial
│           ├── learned_state.*   What survives a power cycle
│           ├── cycle.*           FT8/JS8 slot phase
│           ├── nets.*            Scheduled net directory
│           ├── morse.*           CW decode
│           ├── timezone.*        US zone rules with real DST
│           ├── dial.*            Frequency → band selection
│           ├── hal.h             The hardware seam (interfaces)
│           ├── display.*         The §5 display lines
│           └── app.*             AirTimeApp — wires it all to the seam
├── lib/airtime_esp32/         ESP32 adapters: RDS chip, ADC sampler, SoftAP, NVS
├── test/                     20 suites, 174 cases + fakes.h, a simulated ATS Mini
│                             (one shared tuner, because the device has one)
├── tools/
│   ├── build_fw.sh        Compile a firmware flavour (airtime|fast|stock|probe|survey)
│   ├── bump_version.sh    Advance vYYYY.MM.DD.NNN
│   ├── release.sh         Merged single-file image for a web flasher
│   ├── airtime-sync.sh    Point macOS / Linux / Windows at the radio
│   ├── dt_report.py       Score a WSJT-X log's DT column (Milestone 5)
│   ├── eibi_embed.py      Compile the EiBi shortwave schedule into the image
│   ├── nets_import.py     Import the net directory
│   ├── make_icon.py       Regenerate the phone app's home-screen icon
│   ├── inspect_flash.py   Validate / compare ESP32 flash images (Milestone 0)
│   ├── survey_log.py      Timestamp the radio's serial output to a log
│   └── survey_report.py   Score surveyed FM stations by clock-time accuracy
├── firmware/
│   ├── ats-mini/          Upstream esp32-si4732/ats-mini (git subtree) + AirTime glue
│   └── backup/            Verified stock firmware image + checksum
└── docs/
    ├── PLAN.md            The canonical v1 specification and build plan
    ├── ARCHITECTURE.md    The core ↔ hardware seam
    ├── MILESTONE0.md      Backup / recovery-drill / IO11 runbook (run before flashing)
    ├── MILESTONE1.md      FM station survey runbook
    ├── MILESTONE5.md      Field acceptance runbook and how to measure it
    ├── CLIENT_SETUP.md    Pointing macOS / Linux / Windows at the radio for WSJT-X
    └── STATUS.md          Live milestone / task tracker
```

The `ats-mini` firmware base is vendored as a **git subtree** at `firmware/ats-mini/`,
alongside the verified stock-firmware backup in `firmware/backup/`. Note it builds with
**Arduino CLI** (`ats-mini/sketch.yaml`), not PlatformIO — see
[`docs/MILESTONE0.md §5`](docs/MILESTONE0.md).

## References

- Fork base — https://github.com/esp32-si4732/ats-mini · docs https://esp32-si4732.github.io/ats-mini/
- HJBerndt firmware & IO11/Goertzel documentation — http://www.hjberndt.de/dvb/pocketSI4735DualCoreDecoder.html
- V4 reverse engineering (Peter Neufeld, V4XXL) — https://peterneufeld.wordpress.com/2025/12/05/v4xxl-mini-radio-to-the-max/
- WWV/WWVH signal format (NIST) — https://www.nist.gov/pml/time-and-frequency-division/time-services/
- EiBi shortwave schedule — http://www.eibispace.de/ (embedded; regenerate with `tools/eibi_embed.py --fetch`)

Full reference list in [`docs/PLAN.md §8`](docs/PLAN.md#8-references).

## Credit and licence

Built on [`esp32-si4732/ats-mini`](https://github.com/esp32-si4732/ats-mini) by
the ats-mini contributors, itself descended from the work of PU2CLR (Ricardo
Caratti), G8PTN and others. AirTime's additions follow the upstream licence;
the stock build remains byte-identical so upstream is always recoverable from
this tree.
