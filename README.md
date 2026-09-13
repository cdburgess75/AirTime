<div align="center">

# ⏱ AirTime

### When the internet is gone and GPS can't be trusted, the time still arrives — over the air.

**A multi-source, resilient time reference for FT8/JS8 digital modes, running on an unmodified pocket radio.**
It listens to broadcast FM and to WWV on shortwave, arbitrates them into a drift-disciplined clock, and serves that clock to your laptop as **NTP** — from its own Wi‑Fi access point, with no internet anywhere in the loop.

<br/>

### [**▶  TRY THE APP  ◀**](https://cdburgess75.github.io/AirTime/demo/)

**[cdburgess75.github.io/AirTime/demo](https://cdburgess75.github.io/AirTime/demo/)**

*The real interface, running on simulated data. On an actual radio this is served by the device itself.*

<br/>

[![Firmware](https://img.shields.io/badge/firmware-C%2B%2B17-00599C?logo=cplusplus&logoColor=white)](lib/airtime_core)
[![Platform](https://img.shields.io/badge/ESP32--S3-SI4732-E7352C?logo=espressif&logoColor=white)](#-hardware)
[![Tests](https://img.shields.io/badge/tests-189%20cases%20%2F%206412%20checks-2ea043)](test)
[![Upstream](https://img.shields.io/badge/stock%20build-byte--identical-2ea043)](#-build)
[![Web UI](https://img.shields.io/badge/web%20app-zero%20dependencies-635BFF)](#-save-it-to-your-phone)
[![Base](https://img.shields.io/badge/built%20on-ats--mini-informational)](https://github.com/esp32-si4732/ats-mini)

<br/>

![The AirTime clock face on the ATS Mini V4: large seven-segment local time in amber over green UTC, a confidence line reading plus-or-minus 31 ms with sources RDS+WWV, the tuned FM station, and the FT8 cycle bar filling across the bottom of the panel](docs/img/hero-device.jpg)

> **📸 Drop in:** a hero photo of the radio on the bench, screen lit, clock synced.
> Landscape, ~1600 px wide. This is the first thing anyone sees — make it the device doing its job.

</div>

---

## 🌩 Why this exists

FT8 and JS8 need the operating computer's clock within about a second of UTC — under 200 ms is effectively perfect. In the field, or after a hurricane, there is no internet NTP. GPS is a single point of failure: jammable, spoofable, or simply not in the bag.

Every commercial "field time server" is GPS-only, and none of them arbitrate multiple over-the-air sources or report honest uncertainty. AirTime is the missing device — and it runs on a $60 pocket radio with **no soldering and no modifications**.

---

## ✨ Features

**Time, from the air**
- **📻 Two independent sources, either sufficient.** FM RDS clock-time supplies date and coarse time where FM reaches. On shortwave, WWV supplies **both jobs by itself**: the 100 Hz timecode carries minute/hour/day/year — decoded, and believed only when two whole frames agree in exact lockstep — and the 1000 Hz minute marker then pulls the phase onto the second. A big antenna and no FM dial is a working configuration now. (And if all else fails: Settings → Set Clock, dial the next minute, press at :00.)
- **🗳 Multi-station voting.** Many FM stations send no clock-time, and some send it *wrong*. Stations vote; outliers lose.
- **⚖️ An arbiter that never lies.** Sources steer the clock, they never step it. Corrections over 500 ms need two independent sources agreeing — the defence against a bad station and against spoofing.
- **📉 It learns its own crystal.** Rate error is measured across syncs and stored, so an hourly listen is enough to hold discipline between them.
- **🎚 Uncertainty is a first-class value.** Displayed always, served as real NTP root dispersion, and the clock flags itself **UNSYNCED** the moment the estimate goes stale. It would rather admit doubt than be confidently wrong.

**Things a clock this accurate makes possible**
- **📶 FT8/JS8 cycle instrument.** A bar fills across the current transmit slot with a millisecond countdown — FT8, FT4, **FT2** (3.75 s), JS8 ×4, JT65, WSPR. The fill colour follows *slot parity*, so the TX/RX alternation is visible at a glance. It's also a check on your laptop: if WSJT‑X starts transmitting when the bar hasn't rolled over, your computer's clock is wrong.
- **📱 A phone app, served by the radio.** Add it to your home screen and get a full-screen clock, live cycle bar and diagnostics — rendering local time from *the radio's* UTC, never the phone's.
- **🗓 Net schedules.** ~15 HF nets with published times, so the screen answers *"is it on **now**"* rather than *"what frequency is it on"*. Pick one and it tunes there.
- **🌍 8,142 shortwave stations, embedded.** The full EiBi schedule compiled into the image — because this radio's network has no route to the internet, by design. Tune around and stations name themselves.
- **📟 CW copy & waterfall.** A Morse decoder with a tuning bar, and the SSB passband as live spectrum plus history.

<div align="center">

![Three-panel view of the AirTime screens: the clock face with the FT8 cycle bar, the CW copy terminal showing decoded Morse with its tuning bar, and the waterfall showing the SSB passband as spectrum and history](docs/img/screens-triptych.png)

> **📸 Drop in:** three device screenshots side by side — clock, CW copy, waterfall.
> ~1600 × 600 px. Photograph the panel straight-on in even light.

</div>

---

## 📡 Hardware

**AMNVOLT ATS Mini V4** — unmodified, no soldering. Confirmed a **V4a**: the factory audio tap to IO11 is present and measured.

| | |
|---|---|
| **MCU** | ESP32‑S3‑WROOM‑1‑N16R8 · dual core · 16 MB flash · 8 MB PSRAM |
| **Receiver** | SI4732‑A10 DSP · 150 kHz–30 MHz AM/SSB · 64–108 MHz FM with hardware RDS |
| **Panel** | 320 × 170 TFT · one push-encoder · LiPo · SMA · USB‑C |

Two silicon facts shape the entire design:

> **ADC2 cannot be read while Wi‑Fi is powered.** So every WWV listening window happens with the access point *down* — the radio goes off the network to hear the tone, and comes back by itself. That is the device working correctly, and it says so on screen.

> **There is one tuner.** FM (for RDS) and AM (for WWV) are mutually exclusive. Forgetting that has been the single most productive bug class in this project.

---

## 🚀 Quick start

> **Heads up:** this is **embedded firmware**, not a Node app — there's no `npm install`. The device build uses Arduino CLI; the platform-independent core builds and tests with plain `make`.

### Prerequisites

| | |
|---|---|
| [Arduino CLI](https://arduino.github.io/arduino-cli/) | device build |
| `g++` (C++17) + `make` | host core & tests |
| Python 3 | schedule, icon and release tooling |
| An ATS Mini V4 | …optional. The core and its 189 tests run anywhere. |

### 1 · Clone

```bash
git clone https://github.com/cdburgess75/AirTime.git
cd AirTime
```

### 2 · Run the test suite (no hardware needed)

```bash
make test          # 189 cases · 6,412 checks
```

This exercises the arbiter, the RDS and WWV decoders, drift learning, the NTP
server and a simulated ATS Mini — including a fake with **one shared tuner**,
because the real device has one and pretending otherwise hid two live bugs.

### 3 · Build the firmware

```bash
tools/build_fw.sh airtime     # the AirTime build
tools/build_fw.sh stock       # unmodified upstream, for comparison
```

### 4 · Flash

```bash
tools/release.sh              # -> dist/airtime-<version>-airtime.bin
esptool.py --chip esp32s3 write_flash 0x0 dist/airtime-<version>-airtime.bin
```

One merged image — bootloader, partition table and application at offset 0 — which is what a web flasher expects and removes the commonest way a first flash goes wrong.

### 5 · Point your computer at the radio

Join the radio's **`AirTime`** Wi‑Fi, then:

```bash
./tools/airtime-sync.sh                    # sync once
sudo ./tools/airtime-sync.sh --install     # ...or track it forever
```

It refuses to sync from a radio reporting itself unsynchronised, and does nothing at all when the radio isn't there — which is what makes the installed version safe to forget about. Per-OS detail in [`docs/CLIENT_SETUP.md`](docs/CLIENT_SETUP.md).

Confirm in WSJT‑X: the **DT** column should cluster near zero.

---

## 📱 Save it to your phone

The radio serves its own app. Add it to your home screen and it opens full-screen with its own icon — no browser chrome, no app store, no internet.

<div align="center">

![The AirTime phone app on an iPhone home screen next to other app icons, then opened full-screen showing the large clock, the SYNCED badge and the blue FT8 cycle bar](docs/img/phone-install.png)

> **📸 Drop in:** two phone screenshots side by side — the icon on your home screen, and the app open.
> Portrait, ~1200 px wide combined.

</div>

### First, join the radio

1. On your phone, open **Wi‑Fi settings**
2. Join the network named **`AirTime`**
3. Ignore "no internet connection" — that's the point. This network doesn't go anywhere.
4. Open your browser and visit **`http://192.168.4.1`**

### 🍎 iPhone & iPad — Safari

1. Tap the **Share** button (the square with an arrow, at the bottom)
2. Scroll down and tap **Add to Home Screen**
3. Name it **AirTime** and tap **Add**

> Must be **Safari** — Chrome on iOS can't add to the home screen.

### 🤖 Android — Chrome

1. Tap the **⋮** menu (top right)
2. Tap **Install app**, or **Add to Home screen**
3. Confirm with **Install**

### What you get

| | |
|---|---|
| 🕐 **Big clock** | local and UTC, from *the radio's* time — your phone's clock is never consulted |
| 📶 **Live cycle bar** | the FT8/JS8 slot, animated, with a millisecond countdown |
| 🎛 **Mode picker** | change the cycle mode from your pocket |
| 📊 **Diagnostics** | sources, signal, NTP clients, uptime |

> **⚠️ One honest limitation.** The app needs the radio's access point up to load. There's no offline cache, because service workers require a secure context and this is plain HTTP on an IP address — no certificate is obtainable for an island network. The app detects this and tells you plainly: **the radio goes off the air during every WWV listening window**, and comes back by itself.

---

## 🖥 What the radio serves

| Path | |
|---|---|
| `udp/123` | **NTP**, with honest root dispersion — the real uncertainty, not a fiction |
| `/` | the phone app |
| `/status` | the full diagnostic table — first place to look when something is wrong |
| `/api` | the same state as flat JSON |

<div align="center">

![The AirTime diagnostic status page in a desktop browser, showing confidence, per-source detail, receiver state and NTP client counts in a dark monospace table](docs/img/status-page.png)

> **📸 Drop in:** a desktop browser screenshot of `192.168.4.1/status`.
> ~1400 px wide.

</div>

---

## 🧭 Project status

| Milestone | State |
|---|---|
| **0 — Safety net + hardware verify** | ✅ Backup, recovery drill, IO11 tap confirmed |
| **1 — RDS clock** | ✅ On device — cold start to sync in ~30 s |
| **2 — Serve** | ✅ Laptop running FT8 off the radio's AP, no internet |
| **3 — WWV phase lock** | ✅ 15 MHz minute marker detected and applied · `sntp` → **−0.005 s ± 0.071** |
| **4 — Arbiter + confidence** | ✅ Slew/step, corroboration, per-source drift, honest uncertainty |
| **5 — Field acceptance** | 🟡 An evening of FT8 with median DT inside ±0.2 s — pending |

Tracked in [`docs/STATUS.md`](docs/STATUS.md); the spec is [`docs/PLAN.md`](docs/PLAN.md).

---

## 🐞 The bug class worth knowing about

Nearly every field failure here has been one shape: **something held a cached claim about the radio, and the radio had moved.**

- The test fakes modelled FM and AM as two independent radios. The device has **one**. Giving the fakes a single shared tuner surfaced two real bugs within minutes — including listen windows sampling FM programme audio while the log confidently printed a WWV frequency.
- AirTime tuned the chip directly and never updated the firmware's `currentMode`, so the S-meter was drawn on the shortwave curve while the chip sat on FM — six bars from an ordinary station — and the AGC table applied belonged to a different band entirely. It presented as *"it just searches, but the signal is strong."* The strong signal was the artifact.
- An attenuator set once in the AGC menu, months earlier, persisted in NVS and throttled the front end on every RDS harvest. **RSSI 14 dBµV wasn't the antenna. It was a listening preference applied to a measurement.**

The design answer runs through the code: one function owns tuning, cached claims are voided when the dial moves, and the status page prints the chip's own story next to ours so a disagreement is *visible* rather than inferred.

---

## 🗂 Repository layout

```
AirTime/
├── lib/airtime_core/      Platform-independent core — no Arduino, no ESP-IDF
│   └── src/airtime/
│       ├── arbiter.*         the heart: multi-source arbitration
│       ├── rds_ct.*          RDS group 4A clock-time
│       ├── wwv_marker.*      WWV minute-marker gate
│       ├── wwv_timecode.*    WWV 100 Hz BCD date/time
│       ├── station_vote.*    multi-station voting
│       ├── disciplined_clock.*  slew/rate-steered clock
│       ├── drift.*           crystal learning
│       ├── cycle.*           FT8/JS8 slot phase
│       ├── sntp.*            NTP server
│       └── app.*             wires it all to the hardware seam
├── lib/airtime_esp32/     ESP32 adapters: RDS chip, ADC sampler, SoftAP, NVS
├── test/                  20 suites · 189 cases · a simulated ATS Mini
├── tools/                 build, release, schedule, survey, sync
├── firmware/ats-mini/     upstream (git subtree) + AirTime glue
└── docs/                  PLAN · ARCHITECTURE · STATUS · CLIENT_SETUP · demo/
```

---

## 🔒 The upstream contract

Every change to upstream files sits inside `#ifdef AIRTIME`, and **`tools/build_fw.sh stock` must produce a byte-identical image to unmodified upstream.** That number is checked on every commit. When it moves, something leaked out of the guard and the change is wrong until it moves back.

That's what keeps stock ats-mini recoverable from this tree, always.

Versions are `vYYYY.MM.DD.NNN`. The running version is on the **About** screen, in the phone app's header, and in the serial banner.

---

## 📚 References

- Fork base — [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini) · [docs](https://esp32-si4732.github.io/ats-mini/)
- [HJBerndt](http://www.hjberndt.de/dvb/pocketSI4735DualCoreDecoder.html) — IO11 tap & Goertzel documentation
- [Peter Neufeld](https://peterneufeld.wordpress.com/2025/12/05/v4xxl-mini-radio-to-the-max/) — V4 reverse engineering
- [NIST](https://www.nist.gov/pml/time-and-frequency-division/time-services/) — WWV/WWVH signal format
- [EiBi](http://www.eibispace.de/) — shortwave schedule

---

<div align="center">

**Built on [ats-mini](https://github.com/esp32-si4732/ats-mini)**, descended from the work of PU2CLR, G8PTN and the ats-mini contributors.
AirTime's additions follow the upstream licence, and the stock build stays byte-identical so upstream is always recoverable.

<sub>73</sub>

</div>
