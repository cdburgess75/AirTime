//
// AirTime integration — the whole device, wired to the real radio.
//
// Compiled out unless -DAIRTIME is passed (see docs/MILESTONE0.md for the
// build command). With the flag, this file instantiates the AirTime app and
// its five hardware adapters, and ats-mini.ino hands over three things:
// RDS polling, WiFi ownership, and a slice of every loop() pass. The stock
// firmware otherwise keeps running — display, encoder, battery — which is
// deliberate for bring-up: familiar UI on top, AirTime underneath, reported
// over the USB serial console every 10 s.
//
// ── What "AirTime owns the radio" means in this build ───────────────────────
//  * Tuning: the scheduler retunes at will (FM station rotation, WWV bands).
//    The stock UI's frequency display goes stale and knob tuning gets undone
//    at the next dwell — known v1 roughness, resolved when the §5 display
//    lands.
//  * WiFi: bringUp/tearDown is exclusively AirTime's (the ADC2 rule). Stock
//    netInit/netTickTime are compiled out under -DAIRTIME. Set ats-mini's
//    Wi-Fi setting to Off anyway; that is the state the settings file should
//    persist.
//  * Bluetooth: not blocked in this build, but leave BLE Off in settings —
//    whether an active BLE radio disturbs the IO11 tap is unverified. The
//    Milestone 0 measurements were taken with BLE idle.
//
// ── Hardware facts this glue encodes ────────────────────────────────────────
//  * SI4732 GPIO1 switches the antenna input path between FM and AM/SW (see
//    useBand in ats-mini.ino, credit G8PTN). Forgetting it means WWV silence.
//  * The IO11 audio tap is downstream of the DSP volume control: Milestone 0
//    measured tone_power 1.9e-3 at volume 35. WWV listening therefore forces
//    volume 35 and restores the user's volume afterwards. Yes, the radio is
//    audible while it listens; for a time receiver that is honest behaviour,
//    and the tone is the sound of it working.
//  * AM channel bandwidth is set to 3 kHz explicitly rather than trusting
//    band-table defaults: the 1000 Hz marker tone must pass, and the level
//    calibration (Milestone 3) needs a reproducible filter setting. AVC is
//    left at chip defaults for the same reason the calibration exists at all.

#ifdef AIRTIME

#include "Common.h"
#include "Utils.h"  // loadSSB/unloadSSB — the SSB patch state the chip needs
#include "Menu.h"   // getCurrentUTCOffset(), utcOffsets[] — the device's own
                    // timezone setting, so local time is adjustable in the field
#include "EIBI.h"   // eibiInstallEmbedded — the schedule ships inside the image

#include <WiFi.h>

// The two umbrella includes are what arduino-cli's library discovery keys on;
// the airtime/... prefixed headers only resolve because of them.
#include <airtime_core.h>
#include <airtime_esp32.h>

// ── Milestone 1 station survey ──────────────────────────────────────────────
// SI4735 native FM units (10 kHz): 8990 = 89.9 MHz. Measured 2026-07-26 at
// the home QTH (New Orleans dial), two runs ~35 min apart, offsets stable
// between runs; full table in docs/STATUS.md. These three cluster within the
// voter's tolerance and sit inside RDS's ±250 ms error model. Every other CT
// sender on this dial measured 0.9 s to 6.2 hours from truth — excluded.
// This list is a WARM START for this location, not the mechanism: the
// self-survey design (STATUS.md, "Field variability") supersedes it.
// ── CORRECTED FOR THE ACTUAL QTH ────────────────────────────────────────────
// The previous list was surveyed against the NEW ORLEANS dial. The radio lives
// in Loranger, Tangipahoa Parish (ZIP 70446) — about fifty miles north of it.
// Every station in that list was therefore a fringe signal, which is the best
// explanation anyone has offered for RSSI 14 dBuV on what should be a strong
// local, and for a clock that synced on a good night and not otherwise.
//
// 89.3 first: WRKF Baton Rouge is NPR, 28 kW, with a documented 60-mile radius
// that explicitly covers Hammond and Ponchatoula — Loranger sits inside it. US
// public stations are the most reliable RDS clock-time senders there are, and
// the old list had the device sitting on 89.9 (New Orleans NPR, distant) with
// 89.3 six-tenths of a megahertz away the whole time.
//
// These are RESEARCHED, not measured. The three offsets that used to be
// documented here were real measurements and are gone with the frequencies
// they belonged to; nothing below carries a learned bias yet. Run Survey Dial
// at the bench to replace this with fact — that is what it is for.
static const int32_t kFmStations[] = {
    8930,   // 89.3  WRKF   Baton Rouge NPR, 28 kW, covers Tangipahoa Parish
    10710,  // 107.1 WHMD   Hammond — genuinely local, ~10 miles
    8990,   // 89.9  WWNO   New Orleans NPR — kept: it has produced CT here
};
static const size_t kFmStationCount =
    sizeof(kFmStations) / sizeof(kFmStations[0]);

// WWV band order — a warm start like the FM list above. 15 MHz first: it is
// the only band that has produced a marker at this QTH (two sessions of logs),
// which matches §4's daytime expectation (10/15 by day, 5 at night). The
// scheduler's learned per-band preference takes over once anything delivers;
// this only decides where a fresh boot looks FIRST, so the first window is
// spent on the likeliest band instead of sweeping dead ones.
//
// 20 MHz joined when the timecode chain made unseeded listening real: §4
// always allowed it, and at ~1000 miles from Fort Collins it is often the
// strongest daytime path of all. Last in the sweep — unproven here — but a
// band the rotation can now discover instead of never trying. (2.5 MHz stays
// out: 2.5 kW into a groundwave-scale distance is not a Louisiana signal.)
static const int32_t kWwvBands[] = {15000, 10000, 5000, 20000};
static const size_t kWwvBandCount = sizeof(kWwvBands) / sizeof(kWwvBands[0]);

// ── Settings the operator can actually reach ────────────────────────────────
// Everything below was a compile-time constant until the menu landed, which
// meant a change of location — or of mind — needed a laptop, a toolchain and a
// reflash. A field instrument should not require its own build environment.
//
// Choices persist in the AirTime NVS namespace rather than ats-mini's settings
// blob, so this build stays additive to upstream and a stock reflash cannot
// silently reinterpret bytes it does not know about.

static const airtime::TimeZoneRule* const kZones[] = {
    &airtime::kZoneEastern, &airtime::kZoneCentral, &airtime::kZoneMountain,
    &airtime::kZoneArizona, &airtime::kZonePacific, &airtime::kZoneAlaska,
    &airtime::kZoneHawaii,  &airtime::kZoneUtc,
};
static const char* const kZoneNames[] = {
    "Eastern", "Central", "Mountain", "Arizona",
    "Pacific", "Alaska",  "Hawaii",   "UTC",
};
static const int kZoneCount = sizeof(kZones) / sizeof(kZones[0]);
static int atZone = 1;   // Central; the owner's QTH, and a sane default

// Which WWV band to try FIRST. "Auto" leaves the learned preference alone,
// which is the right answer once the radio has heard anything at all; the
// explicit choices are for a fresh location where waiting out a sweep of dead
// bands is just lost time.
static const char* const kBandNames[] = {"Auto", "15 MHz", "10 MHz", "5 MHz", "20 MHz"};
static const int32_t kBandKhz[] = {0, 15000, 10000, 5000, 20000};
static const int kBandOptCount = sizeof(kBandKhz) / sizeof(kBandKhz[0]);
static int atBandOpt = 0;

// The §5 operator actions. Implemented and tested since Milestone 4 and until
// now reachable from nothing at all.
static const char* const kHfNames[] = {"Listen Now", "Serve Now", "Survey Dial"};

// Clock or receiver. The clock is what this device IS, so it is what every
// power-on comes up as; operator mode is a thing you ask for and it is not
// remembered. While it is on, AirTime touches nothing — see
// AirTimeApp::setMode for why an hour of listening costs milliseconds.
static const char* const kModeNames[] = {"Clock", "Radio", "CW Copy", "Waterfall"};

// (The CW/WWV detector parameters — 700 Hz in 5 ms blocks vs 1000 Hz in 20 ms —
// live in AppConfig now: AirTimeApp::setMode repoints the sampler itself, in a
// sequence the host tests exercise, so this file no longer has an ordering to
// get right.)
static int atModeOpt = 0;
static int atHfOpt = 0;

// ── The digital-mode cycle instrument ───────────────────────────────────────
// 0 = off; 1..kCycleModeCount selects airtime::kCycleModes[opt-1].
//
// This is what the encoder does on the clock face. It used to call the stock
// tuning path, which retuned the chip off the station AirTime was harvesting
// AND wrote the new frequency into the operator's saved band — undone at the
// next dwell rotation up to 75 seconds later, silently, with RDS decode dead
// in the meantime. The one knob on a time appliance should not be a booby
// trap; it should show you the thing the appliance is FOR.
//
// Off by default, so a clock nobody has touched still reads like a clock.
// Persisted, because an operator running FT8 all evening should not have to
// re-choose it after every power cycle.
static int atCycleOpt = 0;

// ── Nets worth knowing about ────────────────────────────────────────────────
// The feature the clock earns: a receiver that knows UTC to milliseconds can
// answer "is it on NOW", not merely "what frequency is it on".
//
// Imported from the SkyWave built-in directory (cdburgess75/SkyWave, v2026.
// 07.26.038) — a hand-curated list compiled from the nets' own published
// schedules, biased toward national traffic/service nets plus Southeast-US
// coverage. **VERIFY AGAINST THE NET'S OWN PAGE BEFORE OPERATIONAL USE**: it
// carries no upstream feed, so times drift as nets change them.
//
// ── Why these are not the UTC times SkyWave publishes ───────────────────────
// SkyWave's table quotes UTC, and warns that its UTC columns are anchored to US
// DAYLIGHT time and shift +1 h in winter — its local-anchor column is the
// authoritative value. Copying the UTC numbers would have made this display an
// hour wrong from November to March, which on a device built to be right about
// time is the one mistake it cannot afford. So each net is stored in the frame
// its schedule was actually written in, and airtime/nets.cpp converts using the
// same DST rule the clock display uses.
//
// Where SkyWave gave no local anchor (ECARS, MIDCARS, both marked "hours
// approximate"), the daylight-time UTC window lands on round local hours —
// 0800-2200 Eastern and 0800-1800 Central — which is what a locally-anchored
// net looks like, so they are recorded that way.
//
// Names are <=11 characters because that is what the side bar can draw; the
// dial frequency and mode go on the detail line instead (atNetDetail).
//
// The two W1AW CW entries that used to sit here are gone. Their times were a
// plausible-looking guess of mine, not a published schedule, and a wrong time
// on this screen is worse than a missing one.
// ── Order is load-bearing ───────────────────────────────────────────────────
// netActiveAt() returns the FIRST listed net that is on the air, and that one
// answer is what the clock screen shows. The wide-area service nets run 10 to
// 14 hours a day, so listing them first would mean the screen reads "NOW ECARS"
// through every regional net's hour and the short, scheduled nets — the ones
// worth being told about — would never once appear. Shortest sessions first,
// therefore: most specific wins.
//
// The same order drives the menu, where regional-first also suits a Gulf Coast
// operator. Reorder freely if the QTH changes; nothing else depends on it.
static const airtime::HamNet kNets[] = {
  // name           kHz   mode                  days              start (local)  mins  anchor
  // ── southeast US: one hour each, on a schedule you can set a watch by ──
  {"FL Phone",     3940, airtime::NetMode::Lsb, airtime::kDaily,   7*60,           60, airtime::NetAnchor::UsEastern},
  {"Waterway",     7268, airtime::NetMode::Lsb, airtime::kDaily,   7*60 + 45,      60, airtime::NetAnchor::UsEastern},
  {"FL Midday",    7242, airtime::NetMode::Lsb, airtime::kDaily,  12*60,           60, airtime::NetAnchor::UsEastern},
  {"MS Phone",     3862, airtime::NetMode::Lsb, airtime::kDaily,  18*60,           60, airtime::NetAnchor::UsCentral},
  {"LA Traffic",   3910, airtime::NetMode::Lsb, airtime::kDaily,  18*60 + 30,      60, airtime::NetAnchor::UsCentral},
  {"AL Traffic",   3965, airtime::NetMode::Lsb, airtime::kDaily,  18*60 + 30,      60, airtime::NetAnchor::UsCentral},
  {"TN Phone",     3980, airtime::NetMode::Lsb, airtime::kDaily,  18*60 + 30,      60, airtime::NetAnchor::UsCentral},
  {"GA SSB",       3975, airtime::NetMode::Lsb, airtime::kDaily,  19*60,           60, airtime::NetAnchor::UsEastern},
  {"SC SSB",       3915, airtime::NetMode::Lsb, airtime::kDaily,  19*60,           60, airtime::NetAnchor::UsEastern},
  // ── wide-area service nets: long windows, informal start ──
  {"SouthCARS",    7251, airtime::NetMode::Lsb, airtime::kDaily,   8*60,          300, airtime::NetAnchor::UsEastern},
  {"Intercon",    14300, airtime::NetMode::Usb, airtime::kDaily,  11*60,          300, airtime::NetAnchor::Utc},
  {"MIDCARS",      7258, airtime::NetMode::Lsb, airtime::kDaily,   8*60,          600, airtime::NetAnchor::UsCentral},
  {"MMSN",        14300, airtime::NetMode::Usb, airtime::kDaily,  16*60,          600, airtime::NetAnchor::Utc},
  {"ECARS",        7255, airtime::NetMode::Lsb, airtime::kDaily,   8*60,          840, airtime::NetAnchor::UsEastern},
  // Listed, never scheduled: activated only for an Atlantic tropical system.
  {"Hurricane",   14325, airtime::NetMode::Usb, airtime::kOnDemand,    0,         1440, airtime::NetAnchor::Utc},
};
static const size_t kNetCount = sizeof(kNets) / sizeof(kNets[0]);

static const uint8_t kWwvListenVolume = 35; // the level Milestone 0 calibrated

static airtime_esp32::EspMonotonicClock atMono;
static airtime_esp32::Esp32RdsSource atRds;
static airtime_esp32::Esp32WwvSampler atWwv;
static airtime_esp32::Esp32WiFiControl atWifi;
static airtime_esp32::NvsTimeStore atStore;
static airtime::AirTimeApp* atApp = nullptr;
static bool atWasListening = false;
static uint8_t atUserVolume = kWwvListenVolume;  // the operator's own setting
static uint32_t atLastReport = 0;

// ── Chip glue ───────────────────────────────────────────────────────────────

// The single-tuner rule, extended to the firmware AirTime lives inside.
//
// The core learned that there is one dial (see AirTimeApp::tuneRds). The glue
// had not: these functions drove the SI4732 directly and left stock's
// `currentMode` describing whatever band the operator last selected. Stock
// branches on that global constantly, so the rest of the firmware went on
// reasoning about a radio that no longer existed:
//
//   * getStrength() picks the HF dBuV->S-point table for currentMode != FM.
//     An ordinary FM station reads ~45 dBuV, which on the HF scale is S9+10 —
//     six bars. The "pegged S-meter over 0 RDS groups" in the field photo was
//     this, not a strong signal. Every reading taken through it was junk.
//   * currentSquelch[currentMode] applies the wrong band's squelch, and can
//     mute the audio the WWV tap is listening to.
//   * doAgc() writes the wrong band's AGC/attenuator table into the front end:
//     the SSB index range is 0..1 where FM's is 0..27, so an SSB "attenuate"
//     lands as a very different setting once the chip is back on FM.
//
// So say what the radio is, then configure it the way selectBand() does for
// that mode. doSoftMute/doAvc are no-ops in FM and are deliberately left off
// the WWV path — soft mute attenuates weak signals, which is precisely the
// minute marker we are straining to hear. doStep is skipped too: it writes a
// clamped step index back into bands[bandIdx], and that band belongs to the
// operator, not to us. The step is passed to setFM/setAM explicitly instead.
// Two things the diagnostic could not previously distinguish, both cheap.
//
// atFmTunes: every atTuneFm() power-cycles the tuner, and RDS needs several
// uninterrupted seconds to acquire block sync. The dwell rotation is meant to
// retune every 75 s; if something is retuning far more often, the decoder would
// never get a fair run at it and the symptom would look exactly like a signal
// too weak to lock. One counter tells the two apart.
//
// atRdsSyncFound: whether the chip has EVER reported acquiring sync, even once
// and even briefly. Zero means the decoder truly never locks; non-zero means it
// locks and loses it, which is a different fault with a different fix.
static uint32_t atFmTunes = 0;
static uint32_t atRdsSyncFound = 0;

static void atTuneFm(int32_t khz10, void*)
{
  ++atFmTunes;
  currentMode = FM;               // the chip is about to be an FM radio: say so
  rx.setFM(6400, 10800, (uint16_t)khz10, 10);
  // setFM() power-cycles the tuner, which resets every property to its default
  // AND wipes any resident SSB patch. Re-apply what FM needs, in stock's order.
  rx.setFMDeEmphasis(fmRegions[FmRegionIdx].value);
  rx.setGpioCtl(1, 0, 0);
  rx.setGpio(0, 0, 0);            // FM antenna path

  // AGC hard on, no attenuation — NOT doAgc(), which would apply whatever the
  // operator last chose in the AGC/ATTN menu.
  //
  // That setting is a LISTENING preference: you attenuate a local blowtorch so
  // it stops splattering. This is not listening. It is a 57 kHz subcarrier at
  // roughly 5% modulation depth being decoded at the edge of lock, and there is
  // no signal level at which discarding front-end gain helps it. A field unit
  // sat at RSSI 14 dBuV, SNR 10 dB, refusing to acquire block sync 1040 polls
  // running — an attenuator inherited from an evening of listening is a way to
  // land exactly there, and it would persist across reboots in NVS while
  // looking like nothing at all.
  //
  // Radio mode is unaffected: selectBand() re-applies the operator's choice on
  // the way in, and this path is only ever reached while AirTime owns the dial.
  // The globals are set alongside so the status page keeps telling the truth.
  disableAgc = 0;
  agcNdx = 0;
  rx.setAutomaticGainControl(0, 0);

  // Let the front end settle before the RDS decoder is configured on top of it.
  // setFM() above power-cycles the tuner; stock's useBand() ends its whole
  // sequence with delay(100) and calls it "wait a bit for things to calm down",
  // which is the kind of comment that is load-bearing more often than it looks.
  // Configuring a decoder into a chip that is still coming up is a good way to
  // have the configuration quietly not take.
  delay(100);

  rx.RdsInit();
  rx.setRdsConfig(1, 2, 2, 2, 2); // chip-level ceiling; adapter gates tighter

  // The patch died with the power cycle above. ssbLoaded is a claim ABOUT the
  // chip, and leaving it set makes the next loadSSB() skip a reload the chip
  // genuinely needs — SSB then plays as AM, which is the "radio mode playing
  // FM audio at 7200" bug arriving by a second route.
  unloadSSB();
}

static void atTuneWwv(int32_t khz, void*)
{
  currentMode = AM;               // WWV is AM, and the S-meter should know it
  rx.setAM(150, 30000, (uint16_t)khz, 5);
  rx.setGpioCtl(1, 0, 0);
  rx.setGpio(1, 0, 0);            // whip/SW antenna path
  rx.setBandwidth(2, 1);          // 3 kHz — the 1000 Hz marker passes cleanly
  doAgc(0);                       // AM's AGC table, for the same reason
  unloadSSB();                    // setAM() power-cycles too
}

static int atRdsRssi(void*)
{
  // Only meaningful in FM mode; during a WWV window the number describes a
  // shortwave band and would mislead a survey that is scanning the FM dial.
  if(!rx.isCurrentTuneFM()) return -1;
  return rx.getCurrentRSSI();
}

static int atRdsRead(uint16_t w[4], uint8_t ble[4], void*)
{
  // The SI4735 library skips the I2C transaction outside FM mode, leaving a
  // stale status struct; the adapter relies on this <0 to never read it. The
  // two refusals are DIFFERENT diagnoses and return distinct codes — see
  // kRdsReadNotFm/kRdsReadNoSync in rds_source.h.
  if(!rx.isCurrentTuneFM()) return airtime_esp32::kRdsReadNotFm;
  rx.getRdsStatus(1, 0, 0);       // INTACK: pop one group, ack the flags
  if(rx.getRdsSyncFound()) ++atRdsSyncFound;
  if(!rx.getRdsSync()) return airtime_esp32::kRdsReadNoSync;
  bool fresh = rx.getRdsReceived() || rx.getNumRdsFifoUsed() > 0;
  if(!fresh) return 0;
  rx.getRdsRawGroup(w, ble);
  return rx.getNumRdsFifoUsed() > 0 ? 2 : 1;
}

// ── Hooks called from ats-mini.ino ──────────────────────────────────────────

// ── Menu accessors (declared in Menu.h) ────────────────────────────────────
// Settings persist as one small blob in AirTime's own NVS namespace. Written
// on change rather than on a timer: menu edits are rare and an operator who
// changes zone then pulls the battery should not lose it.

static void atSaveSettings()
{
  const uint8_t blob[4] = {2, (uint8_t)atZone, (uint8_t)atBandOpt,
                           (uint8_t)atCycleOpt};  // [version, ...]
  atStore.saveBlob("cfg", blob, sizeof(blob));
}

static void atLoadSettings()
{
  uint8_t blob[8];
  size_t n = 0;
  if(!atStore.loadBlob("cfg", blob, sizeof(blob), &n)) return;
  // Version 1 blobs predate the cycle instrument and are still perfectly good
  // — read what they carry and leave the rest at its default, rather than
  // discarding a zone the operator set weeks ago over one missing byte.
  if(n < 3 || (blob[0] != 1 && blob[0] != 2)) return;   // unknown: keep defaults
  if(blob[1] < kZoneCount)    atZone    = blob[1];
  if(blob[2] < kBandOptCount) atBandOpt = blob[2];
  if(blob[0] >= 2 && n >= 4 && blob[3] <= (uint8_t)airtime::kCycleModeCount)
    atCycleOpt = blob[3];
}

int atZoneCount() { return kZoneCount; }
const char *atZoneName(int i) { return (i >= 0 && i < kZoneCount) ? kZoneNames[i] : "?"; }
int atZoneIdx() { return atZone; }
void atSetZoneIdx(int i)
{
  if(i < 0 || i >= kZoneCount || i == atZone) return;
  atZone = i;
  atSaveSettings();
}

int atBandCount() { return kBandOptCount; }
const char *atBandName(int i) { return (i >= 0 && i < kBandOptCount) ? kBandNames[i] : "?"; }
int atBandIdx() { return atBandOpt; }
void atSetBandIdx(int i)
{
  if(i < 0 || i >= kBandOptCount || i == atBandOpt) return;
  atBandOpt = i;
  atSaveSettings();
  // Reorder the rotation so the chosen band is tried first. "Auto" (index 0)
  // restores the surveyed default and lets the learned preference rule.
  if(atApp == nullptr) return;
  if(atBandOpt == 0) { atApp->setWwvBands(kWwvBands, kWwvBandCount); return; }
  int32_t order[kWwvBandCount];
  order[0] = kBandKhz[atBandOpt];
  size_t n = 1;
  for(size_t k = 0 ; k < kWwvBandCount ; k++)
    if(kWwvBands[k] != order[0] && n < kWwvBandCount) order[n++] = kWwvBands[k];
  atApp->setWwvBands(order, n);
}

static int atNetSel = 0;

// ── Nets list ───────────────────────────────────────────────────────────────
// A schedule is only useful if you can look at it. The status line shows the
// one net that matters right now; this shows the whole table with each entry's
// standing, so the answer to "what is on later" does not require waiting.
//
// Names are built into a small rotating set of buffers because the menu
// renderer asks for five entries per frame and holds every pointer until it
// has drawn them all — one shared buffer would draw the same string five times.
const char *atNetName(int i)
{
  // Names ONLY, and short. The stock side bar is built for "Brightness" and
  // "Theme" -- about a dozen characters -- and the first attempt put the name,
  // frequency AND status in each row. On the device that truncated to
  // "e Mobile 14300 NOW" and the zoom overlay ran off the right edge of the
  // panel. Details for the selected net go to atNetDetail() instead, which is
  // the usual list-plus-detail split and fits the space that actually exists.
  static char buf[5][14];
  static uint8_t slot = 0;
  if(i < 0 || (size_t)i >= kNetCount) return "?";
  char *b = buf[slot];
  slot = (slot + 1) % 5;
  snprintf(b, sizeof(buf[0]), "%s", kNets[i].name);
  return b;
}

static const char *atModeLabel(airtime::NetMode m)
{
  switch(m)
  {
    case airtime::NetMode::Lsb: return "LSB";
    case airtime::NetMode::Usb: return "USB";
    case airtime::NetMode::Cw:  return "CW";
    case airtime::NetMode::Fm:  return "FM";
    default:                    return "AM";
  }
}

// The selected net in full, for the roomy area the menu leaves free. The list
// shows names alone -- 11 characters is all the side bar can draw -- so this is
// where the dial frequency, the mode and the timing actually live.
const char *atNetDetail()
{
  static char b[48];
  if((size_t)atNetSel >= kNetCount) return "";
  const airtime::HamNet& n = kNets[atNetSel];
  const char *mode = atModeLabel(n.mode);

  // A net with no schedule has no timing to report and must not be made to look
  // as though it does. SkyWave lists the Hurricane Watch Net as 0000-2400 so it
  // never disappears from its UI; here that would read "ON AIR NOW" every hour
  // of every day for a net that is almost never up.
  if(n.days == airtime::kOnDemand)
  {
    snprintf(b, sizeof(b), "%ld kHz %s  when activated", (long)n.khz, mode);
    return b;
  }

  // No trustworthy clock, no timing claim -- a schedule read off a wrong clock
  // looks right, which is the worst way for this to fail.
  if(atApp == nullptr || !atApp->displayState().synced)
  {
    snprintf(b, sizeof(b), "%ld kHz %s", (long)n.khz, mode);
    return b;
  }

  const int64_t utc_s = atApp->displayState().utc_us / 1000000;
  if(airtime::netActiveAt(&kNets[atNetSel], 1, utc_s) >= 0)
  {
    snprintf(b, sizeof(b), "%ld kHz %s  ON AIR NOW", (long)n.khz, mode);
  }
  else
  {
    int wait = 0;
    airtime::netNextAt(&kNets[atNetSel], 1, utc_s, &wait);
    if(wait >= 60) snprintf(b, sizeof(b), "%ld kHz %s  in %dh%02d", (long)n.khz, mode, wait / 60, wait % 60);
    else           snprintf(b, sizeof(b), "%ld kHz %s  in %dm", (long)n.khz, mode, wait);
  }
  return b;
}

int atNetCount() { return (int)kNetCount; }
int atNetIdx() { return atNetSel; }
void atSetNetIdx(int i) { if(i >= 0 && i < (int)kNetCount) atNetSel = i; }

// Take the dial, without tuning it.
//
// Splitting this out of atSetModeIdx() is what lets a net be tuned in one move
// instead of two: entering operator mode and then retuning would run the whole
// band-select sequence twice, and on an SSB band each pass reloads the patch —
// a second of "Loading SSB" apiece.
static void atEnterRadioMode()
{
  atModeOpt = 1;
  if(atApp != nullptr) atApp->setMode(airtime::OpMode::Radio);

  // ssbLoaded is a cached claim about the chip, and AirTime has been calling
  // setFM/setAM behind its back all along, so the flag cannot be trusted.
  // Clearing it forces the next selectBand() to do a genuine reload.
  unloadSSB();
}

int atModeCount() { return (int)(sizeof(kModeNames) / sizeof(kModeNames[0])); }
const char *atModeName(int i)
{
  if(i < 0 || i >= atModeCount()) return "?";
  if(i != atModeOpt) return kModeNames[i];
  // The mode the radio is IN, as distinct from the row the encoder is on —
  // the two are different things now that scrolling does not switch.
  static char b[16];
  snprintf(b, sizeof(b), "%s *", kModeNames[i]);
  return b;
}
int atModeIdx() { return atModeOpt; }
// ── What this device is, in the order you would ask ─────────────────────────
// Every line here has been wanted during a real field problem: which build is
// on the unit, is the clock trustworthy right now, what is it listening to,
// and does it actually hold the schedule it claims to. Uptime last, because
// it is how you tell a reboot loop from a long quiet run.
int airtimeAboutLines(const char *out[], int max)
{
  static char l[7][64];
  int n = 0;
  if(atApp == nullptr || max <= 0) return 0;

  const airtime::DisplayState st = atApp->displayState();

  char unc[24], src[24], age[16];
  airtime::formatUncertainty(st.uncertainty_us, unc, sizeof(unc));
  airtime::formatSources(st.sources, src, sizeof(src));
  airtime::formatAge(st.since_sync_us, age, sizeof(age));
  // formatUncertainty spends a UTF-8 "±" the TFT fonts do not carry; the
  // screen strings in this file are ASCII for exactly that reason.
  const char *u = unc;
  while(*u && (unsigned char)*u >= 0x80) u++;

  if(!st.clock_valid)
    snprintf(l[n], sizeof(l[n]), "Clock:  no fix yet");
  else if(!st.synced)
    snprintf(l[n], sizeof(l[n]), "Clock:  UNSYNCED, coasting (+/-%s)", u);
  else
    snprintf(l[n], sizeof(l[n]), "Clock:  SYNCED +/-%s, %s ago", u, age);
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "Source: %s        NTP: %d client%s",
           src, st.ntp_clients, st.ntp_clients == 1 ? "" : "s");
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "FM:     %.1f  %.1f  %.1f MHz",
           (double)kFmStations[0] / 100.0, (double)kFmStations[1] / 100.0,
           (double)kFmStations[2] / 100.0);
  out[n] = l[n]; if(++n >= max) return n;

  {
    // All bands in MHz, however many the table carries.
    char b[40];
    int off = 0;
    for(size_t k = 0 ; k < kWwvBandCount && off < (int)sizeof(b) - 4 ; k++)
      off += snprintf(b + off, sizeof(b) - off, "%s%ld", k ? "/" : "",
                      (long)(kWwvBands[k] / 1000));
    snprintf(l[n], sizeof(l[n]), "WWV:    %s MHz  (%s first)", b,
             kBandNames[atBandOpt]);
  }
  out[n] = l[n]; if(++n >= max) return n;

  // The schedule the device HOLDS, not the one the image carries — they differ
  // for exactly as long as an install is failing, which is when you need to
  // know. Counted from the file, so it cannot inherit a stale claim.
  const int entries = eibiEntryCount();
  if(entries > 0)
    snprintf(l[n], sizeof(l[n]), "EiBi:   %d entries, dataset %s",
             entries, kEibiInstalledVersion());
  else
    snprintf(l[n], sizeof(l[n]), "EiBi:   NOT INSTALLED");
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "AP:     AirTime, 192.168.4.1:123");
  out[n] = l[n]; if(++n >= max) return n;

  const uint32_t up = millis() / 1000;
  snprintf(l[n], sizeof(l[n]), "Uptime: %luh %02lum %02lus",
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
           (unsigned long)(up % 60));
  out[n] = l[n]; ++n;
  return n;
}

// ── The HF page of About: what the WWV chain is actually hearing ────────────
// The field debugging loop for this project is a PHOTOGRAPH OF THE SCREEN, so
// the numbers that decide the next move have to be on one. Everything here
// exists because its absence cost a session: the marker page distinguishes
// "no tone" from "tone but wrong duration" from "markers rejected"; the code
// page distinguishes "no pulses" from "pulses but no frame" from "frames that
// never chain". The serial log carries the same story with the raw frame.
int airtimeHfAboutLines(const char *out[], int max)
{
  static char l[7][64];
  int n = 0;
  if(atApp == nullptr || max <= 0) return 0;

  const airtime::WwvMarkerDiag md = atApp->wwvMarker().diag();
  const airtime::WwvMarkerDiag pd = atApp->wwvPulse().diag();
  const airtime::WwvTimecodeDecoder& tc = atApp->wwvTimecode();
  const airtime::WwvTimecodeDiag& td = atApp->wwvTimecodeDiag();

  const long band = (long)atApp->directive().wwv_band_khz;
  if(band > 0)
    snprintf(l[n], sizeof(l[n]), "WWV:    listening on %ld kHz", band);
  else
    snprintf(l[n], sizeof(l[n]), "WWV:    not in a listen window");
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "Marker: flr %.0e pk %.0e mk %lu",
           (double)md.noise_floor, (double)md.max_power,
           (unsigned long)md.markers);
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "        starts %lu  rej %lu/%lu  run %ldms",
           (unsigned long)md.tone_starts, (unsigned long)md.rejected_short,
           (unsigned long)md.rejected_long, (long)(md.last_tone_us / 1000));
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "Code:   flr %.0e  pulses %lu  gaps %lu",
           (double)pd.noise_floor, (unsigned long)td.pulses,
           (unsigned long)td.gap_seconds);
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "        frames %lu  read %lu  paired %lu",
           (unsigned long)tc.framesSeen(), (unsigned long)tc.framesDecoded(),
           (unsigned long)tc.framesConfirmed());
  out[n] = l[n]; if(++n >= max) return n;

  if(tc.haveFrame())
  {
    const airtime::WwvTime& t = tc.time();
    snprintf(l[n], sizeof(l[n]), "Last:   day %d %02d:%02d UTC %d  %+ldms %c",
             t.day_of_year, t.hour, t.minute, t.year,
             (long)(td.offset_us / 1000), td.accepted ? 'A' : 'R');
  }
  else
    snprintf(l[n], sizeof(l[n]), "Last:   no confirmed code frame yet");
  out[n] = l[n]; if(++n >= max) return n;

  snprintf(l[n], sizeof(l[n]), "Set Clock (Settings) opens this gate too");
  out[n] = l[n]; ++n;
  return n;
}

// ── Manual time set (Settings -> Set Clock) ─────────────────────────────────
// The always-available tier-3 source, finally reachable. The operator dials
// UTC to the NEXT minute, waits, and presses the knob exactly when their
// watch rolls over: ±1-2 s of truth against setManualUtc's honest ±5 s claim.
// That does not make the clock synced — it opens the WWV gate, and the next
// marker or code frame does the rest. Fields wrap; days respect the month,
// leap years included, because an instrument that lets you dial Feb 30 and
// silently means Mar 2 has already lied once.
static int atClkField = 0;    // 0..5: Year Month Day Hour Min GO
static int atClkY = 2026, atClkMo = 1, atClkD = 1, atClkH = 0, atClkMi = 0;

static int atClkDaysInMonth(int y, int mo)
{
  static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return (mo == 2 && leap) ? 29 : d[mo - 1];
}

void atSetClkOpen()
{
  atClkField = 0;
  if(atApp != nullptr && atApp->displayState().clock_valid)
  {
    // Prefill from the clock's own estimate, advanced to a comfortably-future
    // minute so the operator confirms fields instead of typing them. Even an
    // UNSYNCED estimate is the best starting point available — it is being
    // corrected, not trusted.
    const int64_t s = atApp->displayState().utc_us / 1000000 + 90;
    const int64_t min_s = (s / 60) * 60;
    const int64_t days = min_s / 86400;
    int mo = 0, dy = 0, yr = 0;
    airtime::mjdToCivil((int32_t)(days + 40587), &yr, &mo, &dy);
    atClkY = yr; atClkMo = mo; atClkD = dy;
    atClkH = (int)((min_s % 86400) / 3600);
    atClkMi = (int)((min_s % 3600) / 60);
    return;
  }
  // No clock at all: start from the build date. Whatever is dialed from here,
  // the year and month are usually right already.
  static const char m[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const char *bd = __DATE__;             // "Aug  4 2026"
  atClkY = (bd[7] - '0') * 1000 + (bd[8] - '0') * 100 +
           (bd[9] - '0') * 10 + (bd[10] - '0');
  atClkMo = 1;
  for(int i = 0 ; i < 12 ; i++)
    if(bd[0] == m[i][0] && bd[1] == m[i][1] && bd[2] == m[i][2]) atClkMo = i + 1;
  atClkD = (bd[4] == ' ' ? 0 : (bd[4] - '0') * 10) + (bd[5] - '0');
  atClkH = 0;
  atClkMi = 0;
}

int atClkFieldCount() { return 6; }
int atClkFieldIdx() { return atClkField; }

const char *atClkFieldName(int i)
{
  static char buf[6][16];
  if(i < 0 || i >= 6) return "?";
  char *b = buf[i];
  switch(i)
  {
    case 0: snprintf(b, sizeof(buf[0]), "Year  %04d", atClkY); break;
    case 1: snprintf(b, sizeof(buf[0]), "Month %02d", atClkMo); break;
    case 2: snprintf(b, sizeof(buf[0]), "Day   %02d", atClkD); break;
    case 3: snprintf(b, sizeof(buf[0]), "Hour  %02d", atClkH); break;
    case 4: snprintf(b, sizeof(buf[0]), "Min   %02d", atClkMi); break;
    case 5: snprintf(b, sizeof(buf[0]), "GO at :00"); break;
  }
  return b;
}

static int atClkWrap(int v, int lo, int hi)
{
  if(v < lo) return hi;
  if(v > hi) return lo;
  return v;
}

void atClkTurn(int16_t enc)
{
  const int step = enc > 0 ? 1 : -1;
  switch(atClkField)
  {
    case 0: atClkY = atClkWrap(atClkY + step, 2025, 2099); break;
    case 1: atClkMo = atClkWrap(atClkMo + step, 1, 12); break;
    case 2: atClkD = atClkWrap(atClkD + step, 1, atClkDaysInMonth(atClkY, atClkMo)); break;
    case 3: atClkH = atClkWrap(atClkH + step, 0, 23); break;
    case 4: atClkMi = atClkWrap(atClkMi + step, 0, 59); break;
    default: break;   // GO row: rotation does nothing; the click is the act
  }
  // A month or year turn can strand the day on the 31st of a 30-day month.
  const int dm = atClkDaysInMonth(atClkY, atClkMo);
  if(atClkD > dm) atClkD = dm;
}

bool atClkClick()
{
  if(atClkField < 5) { ++atClkField; return false; }

  // THE moment: the operator's watch just rolled onto HH:MM:00.
  if(atApp != nullptr)
  {
    const int64_t days = airtime::civilToMjd(atClkY, atClkMo, atClkD) - 40587;
    const int64_t utc_s = days * 86400 + (int64_t)atClkH * 3600 +
                          (int64_t)atClkMi * 60;
    atApp->setManualUtc(utc_s * 1000000);
  }
  return true;   // close the panel; the clock face now shows the consequence
}

// The encoder, on the clock face. Wraps through OFF and every mode so a full
// turn always gets you home; saved on every change because the alternative is
// an operator discovering after a power cycle that the radio forgot.
int atCycleIdx() { return atCycleOpt; }
int atCycleCount() { return (int)airtime::kCycleModeCount; }

const char *atCycleName(int i)
{
  if(i <= 0) return("Off");
  if(i > (int)airtime::kCycleModeCount) return("?");
  return(airtime::kCycleModes[i - 1].name);
}

long atCyclePeriodMs(int i)
{
  if(i <= 0 || i > (int)airtime::kCycleModeCount) return(0);
  return((long)(airtime::kCycleModes[i - 1].period_us / 1000));
}

void atSetCycleIdx(int i)
{
  if(i < 0 || i > (int)airtime::kCycleModeCount || i == atCycleOpt) return;
  atCycleOpt = i;
  atSaveSettings();
}

bool atCycleTurn(int16_t enc)
{
  if(!enc) return(false);
  const int n = (int)airtime::kCycleModeCount + 1;   // +1 for OFF
  int v = (atCycleOpt + (enc > 0 ? 1 : -1)) % n;
  if(v < 0) v += n;
  if(v == atCycleOpt) return(false);
  atCycleOpt = v;
  atSaveSettings();
  return(true);
}

bool airtimeOwnsDial()
{
  // Clock mode is the only mode in which the dial is AirTime's. CW copy
  // and the waterfall both run on a frequency the operator chose by ear.
  return(atApp != nullptr && atApp->mode() == airtime::OpMode::Clock);
}

bool airtimeRadioMode() { return atApp != nullptr && atApp->radioMode(); }
bool airtimeCwMode()    { return atApp != nullptr && atApp->cwMode(); }
bool airtimeSpectrumMode()
{
  return atApp != nullptr && atApp->mode() == airtime::OpMode::Spectrum;
}
uint32_t atSpectrumCopy(float out[AT_SPECTRUM_BINS])
{
  static_assert(AT_SPECTRUM_BINS ==
                    (int)airtime_esp32::Esp32WwvSampler::kSpectrumBins,
                "display and sampler disagree about the bin count");
  return atWwv.copySpectrum(out);
}

void atResetLearning()
{
  // The order is the safety: wipe, then reboot without another persist tick —
  // a loop pass between the two could write fresh state into the emptiness.
  atStore.wipeAll();
  ESP.restart();
}

// What CW copy has heard, and how well it is hearing it.
const char *atCwText()  { return atApp != nullptr ? atApp->cwText().text() : ""; }
int atCwWpm()           { return atApp != nullptr ? atApp->cwStatus().wpm : 0; }
bool atCwKeyDown()      { return atApp != nullptr && atApp->cwStatus().key_down; }
// Tone against the noise floor, as a percentage of "comfortably copyable".
// The decoder calls the key down at 4x and up again at 2x, so 4x is the number
// that matters; 12x is a strong signal and the top of the bar.
int atCwLevelPct()
{
  if(atApp == nullptr) return 0;
  const float snr = (float)atApp->cwSnr();
  int pct = (int)(100.0f * snr / 12.0f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
void atSetModeIdx(int i)
{
  if(i < 0 || i >= atModeCount() || i == atModeOpt) return;
  atModeOpt = i;
  if(atApp == nullptr) return;

  if(i == 0)
  {
    // Back to being a clock. setMode(Clock) does the remembering-what-went-
    // stale: it re-tunes the FM side and voids the WWV band cache, because the
    // operator has been moving a dial AirTime cannot see. The spectrum bank is
    // dropped after the stop setMode performs — same race-free ordering as
    // entry.
    atApp->setMode(airtime::OpMode::Clock);
    atWwv.disableSpectrum();
    return;
  }

  // Radio and CW both hand the dial to the operator, and that means the WHOLE
  // stock sequence.
  //
  // useBand() alone is not enough and the failure is silent: for an SSB band it
  // calls rx.setSSB(), which does nothing useful unless the SSB patch has been
  // loaded into the SI4735 first. Observed on the device — the display read
  // "41M USB 7200.000" while the speaker kept playing the FM station AirTime
  // had been harvesting. The readout was honest about intent and wrong about
  // reality, which is the exact failure this screen exists to prevent.
  //
  // Entering CW from Clock is the same story with a different ending: AirTime
  // will have been parked on an FM broadcast station harvesting RDS, and
  // without this the operator would open CW copy pointed at music.
  //
  // CW additionally takes the audio tap, which costs the access point — ADC2
  // and the WiFi radio cannot both be live (PLAN.md §2) — so NTP stops
  // answering until the operator leaves, and the screen says so. The tap level
  // follows the DSP volume (Milestone 0) and nothing overrides it here: CW is
  // tuned by ear, so the volume set to hear the note is the level the detector
  // sees, and the decoder's own noise floor adapts to it.
  atApp->setMode(i == 2 ? airtime::OpMode::Cw :
                 i == 3 ? airtime::OpMode::Spectrum : airtime::OpMode::Radio);
  // setMode stopped the sampler synchronously whichever direction we crossed,
  // so flipping the bank here cannot race the task (its contract refuses a
  // running sampler; the return is deliberately unchecked because the stop
  // above makes refusal impossible, and the next applyDirective restarts).
  if(i == 3) atWwv.enableSpectrum(150.0f, 50.0f);
  else       atWwv.disableSpectrum();
  unloadSSB();           // the cached patch state cannot be trusted; see above
  selectBand(bandIdx);   // loadSSB if needed -> useBand -> setBandwidth
  rx.setVolume(volume);
}

// ── Tuning a net ────────────────────────────────────────────────────────────

// The chip has no CW demodulator, so CW is received as SSB and the operator
// hears the beat note. USB by convention: the sideband a CW signal is tuned in
// is a choice, and USB is what the digital-mode world uses on every band.
static uint8_t atChipMode(airtime::NetMode m)
{
  switch(m)
  {
    case airtime::NetMode::Lsb: return LSB;
    case airtime::NetMode::Usb: return USB;
    case airtime::NetMode::Cw:  return USB;
    case airtime::NetMode::Fm:  return FM;
    default:                    return AM;
  }
}

// Which entry of bands[] should hold this dial frequency. -1 if none can, which
// is a table error rather than an operator error.
//
// The CHOICE — narrowest span wins, or "ALL" (150-30000 kHz) swallows every net
// in the directory and tuneToMemory() rewrites its mode — lives in
// airtime/dial.h, host-tested against a copy of this very table. This function
// only translates bands[] rows into spans, and mirrors isMemoryInBand()'s
// FM-vs-not rule exactly so tuneToMemory() cannot refuse what the pick
// promised.
static int atBandForKhz(int32_t khz, uint8_t mode)
{
  // bands[] is extern here with unknown extent, so the buffer is a constant:
  // 32 clears the 28 shipped rows. A grown table clamps rather than overruns —
  // and if that ever happens, the last bands become unreachable to nets, which
  // the -1 path reports on screen instead of hiding.
  constexpr int kMaxSpans = 32;
  airtime::DialBandSpan spans[kMaxSpans];
  int n = getTotalBands();
  if(n > kMaxSpans) n = kMaxSpans;
  for(int i = 0; i < n; i++)
  {
    spans[i].min_khz = bands[i].minimumFreq;
    spans[i].max_khz = bands[i].maximumFreq;
    spans[i].fm = (bands[i].bandType == FM_BAND_TYPE) || (bands[i].bandMode == FM);
    spans[i].mode = bands[i].bandMode;
  }
  return airtime::pickDialBand(spans, (size_t)n, khz, mode == FM, mode);
}

bool atTuneNet(int i)
{
  if(i < 0 || (size_t)i >= kNetCount) return false;
  const airtime::HamNet& n = kNets[i];

  const uint8_t mode = atChipMode(n.mode);
  const int band = atBandForKhz(n.khz, mode);
  if(band < 0) return false;

  // Operator mode FIRST. Tuning while AirTime still owns the dial would work
  // for a few seconds and then be undone without a word: the scheduler retunes
  // to an FM station on its own timetable, and the operator would be left
  // listening to music on a screen that said 14300.
  atEnterRadioMode();

  Memory m;
  memset(&m, 0, sizeof(m));
  m.freq = (uint32_t)n.khz * 1000;
  m.band = (uint8_t)band;
  m.mode = mode;
  return tuneToMemory(&m);
}

int atHfCount() { return (int)(sizeof(kHfNames) / sizeof(kHfNames[0])); }
const char *atHfName(int i) { return (i >= 0 && i < atHfCount()) ? kHfNames[i] : "?"; }
int atHfIdx() { return atHfOpt; }
// The committer: called on CLICK, never while scrolling. This used to act on
// every encoder detent — "these are verbs, §5 asks for them to be immediate" —
// and the field found the flaw in that reading of §5: scrolling the list just
// to SEE the options fired them. Browsing past "Survey Dial" handed the tuner
// to a half-hour survey; past "Listen Now", tore down the access point for a
// listen window. Immediate means "on selection", and selection is a click.
void atSetHfIdx(int i)
{
  if(i < 0 || i >= atHfCount()) return;
  atHfOpt = i;
  if(atApp == nullptr) return;
  if(atHfOpt == 0)      atApp->operatorListenNow();
  else if(atHfOpt == 1)  atApp->operatorServeNow();
  else                   atApp->startSurvey();   // ~30 min; owns the dial
}

// ── Scroll is looking; click is doing ───────────────────────────────────────
// The Mode and HF Listen lists hold ACTIONS, and until this existed they fired
// while the cursor moved across them. Scrolling Clock->CW Copy to look at the
// options live-switched the radio through Radio (a full SSB hand-over per
// detent) into CW (access point down, NTP off the air). The clock got knocked
// over by someone reading a menu. So these lists carry a selection cursor that
// commits only on click; Zone and WWV Band stay live, because scrolling a
// VALUE into place is what a settings knob is for.
static int atModeSel = -1;   // -1: the list opens at the active mode
int atModeSelIdx() { return atModeSel < 0 ? atModeOpt : atModeSel; }
void atSetModeSel(int i) { if(i >= 0 && i < atModeCount()) atModeSel = i; }
void atModeSelReset() { atModeSel = -1; }
void atModeCommit() { atSetModeIdx(atModeSelIdx()); atModeSel = -1; }

static int atHfSel = 0;
int atHfSelIdx() { return atHfSel; }
void atSetHfSel(int i) { if(i >= 0 && i < atHfCount()) atHfSel = i; }
void atHfSelReset() { atHfSel = 0; }
void atHfCommit() { atSetHfIdx(atHfSel); }

void airtimeSetup()
{
  // Status prints must NEVER block: with no computer attached to USB, HWCDC
  // writes stall until a timeout, and a stalled main loop overflows the WWV
  // block queue (observed: 13% of blocks dropped across unlogged listen
  // windows). Zero timeout means "drop the output, keep the radio running" —
  // the right priority for a time appliance.
  Serial.setTxTimeoutMs(0);

  // Known state: radio silent until the scheduler says otherwise. Stock netInit
  // is compiled out in this build, but the chip may still hold AP config.
  WiFi.persistent(false);
  WiFi.mode(WIFI_MODE_NULL);

  atStore.begin();
  atLoadSettings();

  // The EiBi shortwave schedule ships inside this image, because the radio's
  // network has no internet on purpose. First boot after a flash with new
  // data parses it into LittleFS (a few seconds, with a progress screen);
  // every boot after that is a version-string compare.
  eibiInstallEmbedded(false);

  airtime_esp32::RdsSourceConfig rdsCfg;
  airtime_esp32::RdsChipOps rdsOps;
  rdsOps.tune = &atTuneFm;
  rdsOps.read = &atRdsRead;
  rdsOps.rssi = &atRdsRssi;
  atRds.begin(rdsCfg, rdsOps, &atMono);

  airtime_esp32::WwvSamplerConfig wwvCfg;   // GPIO11, 1 kHz, 20 ms blocks
  atWwv.begin(wwvCfg, &atTuneWwv, nullptr);

  airtime_esp32::WiFiControlConfig wifiCfg; // open AP "AirTime", UDP/123
  atWifi.begin(wifiCfg);

  airtime::AppDeps deps;
  deps.clock = &atMono;
  deps.rds = &atRds;
  deps.wwv = &atWwv;
  deps.wifi = &atWifi;
  deps.store = &atStore;

  airtime::AppConfig cfg;
  cfg.wwv_calibration_us = 0;  // measured on this unit in Milestone 3
  // The century for WWV's two-digit year, from the build date — the honest
  // source: it cannot be wrong by less than a hundred years and needs no
  // other reference. __DATE__ is "Mmm dd yyyy"; the year is the tail.
  cfg.century_hint_year = (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 +
                          (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
#ifdef AIRTIME_FAST_LISTEN
  // Test builds only: WWV listen windows every 10 minutes instead of hourly,
  // so marker detection can be verified without waiting out the hour. Not a
  // field configuration — six windows an hour costs serving uptime.
  cfg.scheduler.listen_interval_us = 10LL * 60 * 1000000;
#endif

  atApp = new airtime::AirTimeApp(deps, cfg);
  atApp->setFmStations(kFmStations, kFmStationCount);
  atApp->setWwvBands(kWwvBands, kWwvBandCount);
  atApp->begin();

  Serial.printf("AirTime %s: up. NTP at 192.168.4.1:123 while serving.\n",
                AIRTIME_VERSION);
}

// ── The §5 screen ───────────────────────────────────────────────────────────
// Layout-AirTime.cpp draws; this assembles what it draws. Kept apart so the
// layout file needs no AirTime headers and this file needs no TFT ones.
//
// Everything here is ASCII. The core's display module formats for a terminal
// and spends "±" and "·" freely; the TFT fonts have neither, and the first
// photo back from the device showed them as gaps ("250 ms  RDS  sync 26s
// ago"). Screen strings are therefore built here rather than reused.


// The operator's time zone, as a RULE rather than an offset.
//
// A fixed label is wrong half the year and a fixed offset is wrong the other
// half: Central is CST at UTC-6 in January and CDT at UTC-5 in July. So the
// zone carries both names and its own DST rule, and the display follows the
// calendar without anyone touching a menu twice a year. Swap this one line for
// kZoneEastern / kZoneMountain / kZonePacific / kZoneArizona / kZoneAlaska /
// kZoneHawaii / kZoneUtc — see airtime/timezone.h, which is host-tested
// against the real 2026 transition instants.
#define kLocalZone (*kZones[atZone])

int atLocalOffsetS()
{
  if(atApp == nullptr) return(0);
  const airtime::DisplayState st = atApp->displayState();
  if(!st.clock_valid) return(0);
  const int64_t utc_s = st.utc_us / 1000000;
  const char *zone = "";
  return((int)(airtime::localEpochS(kLocalZone, utc_s, &zone) - utc_s));
}


// ── What the status page needs ──────────────────────────────────────────────
// The adapters and the app are statics in this file. Rather than make them
// globals so one page can read them, hand out exactly what it asks for.
const airtime::AirTimeApp *airtimeApp() { return atApp; }
uint32_t airtimeRdsAccepted()  { return atRds.groupsAccepted(); }
uint32_t airtimeRdsRejected()  { return atRds.groupsRejected(); }
uint32_t airtimeRdsNotFm()     { return atRds.pollsNotFm(); }
uint32_t airtimeRdsNoSync()    { return atRds.pollsNoSync(); }
uint32_t airtimeRdsEmpty()     { return atRds.pollsEmpty(); }
uint32_t airtimeApFailures()   { return atWifi.upFailures(); }
uint32_t airtimeNtpServed()    { return atWifi.requestsServed(); }
int32_t  airtimeRdsTunedKhz()  { return atRds.tunedKhz(); }

// AirTimeScreen is declared in Menu.h — the TFT layout and the web status page
// both render from it.
void airtimeScreen(AirTimeScreen *out)
{
  // Static: the layout holds these pointers only for the length of one draw,
  // but it does read them after this returns.
  static char clockBuf[16]   = "--:--:--";
  static char localBuf[16]   = "--:--:--";
  static char zoneBuf[12]    = "";
  static char statusBuf[64]  = "starting";
  static char tunedBuf[40]   = "";
  static char clientsBuf[24] = "";
  static char netBuf[40]     = "";
  static char diagBuf[48]    = "";
  static char cycleBuf[24]   = "";
  static char cycleTBuf[16]  = "";

  if(atApp == nullptr)
  {
    out->clock = clockBuf; out->local = localBuf; out->zone = zoneBuf;
    out->status = statusBuf;
    out->tuned = tunedBuf; out->clients = clientsBuf; out->net = netBuf;
    out->diag = diagBuf;
    out->cycle = cycleBuf; out->cycle_t = cycleTBuf;
    out->cycle_fraction = 0.0f; out->cycle_odd = false;
    out->synced = false;   out->valid = false;
    return;
  }

  const airtime::DisplayState st = atApp->displayState();

  if(st.clock_valid)
  {
    const int64_t utc_s = st.utc_us / 1000000;
    const int64_t sod = utc_s % 86400;
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d",
             (int)(sod / 3600), (int)((sod % 3600) / 60), (int)(sod % 60));

    // Local time and its name for THIS instant — the name changes with the
    // season, so both come from the same call. Floor-mod because zones west of
    // Greenwich otherwise wrap into a negative hour.
    const char* zone = "";
    const int64_t local_s = airtime::localEpochS(kLocalZone, utc_s, &zone);
    int64_t lsod = local_s % 86400;
    if(lsod < 0) lsod += 86400;
    snprintf(localBuf, sizeof(localBuf), "%02d:%02d:%02d",
             (int)(lsod / 3600), (int)((lsod % 3600) / 60), (int)(lsod % 60));
    snprintf(zoneBuf, sizeof(zoneBuf), "%s", zone);
  }
  else
  {
    snprintf(clockBuf, sizeof(clockBuf), "--:--:--");
    snprintf(localBuf, sizeof(localBuf), "--:--:--");
  }

  // Uncertainty in whichever unit reads naturally, then who has been steering
  // and how long ago. "UNSYNCED" earns the whole line when the estimate has
  // gone stale — that is the one state the operator must not misread.
  char unc[16];
  const int64_t u = st.uncertainty_us;
  if(u >= 10000000)     snprintf(unc, sizeof(unc), "+/-%llds", (long long)(u / 1000000));
  else if(u >= 1000000) snprintf(unc, sizeof(unc), "+/-%.1fs", (double)u / 1e6);
  else                  snprintf(unc, sizeof(unc), "+/-%lldms", (long long)(u / 1000));

  char src[16] = "no source";
  const uint8_t m = st.sources;
  if(m) snprintf(src, sizeof(src), "%s%s%s",
                 (m & airtime::kSrcRds) ? "RDS" : "",
                 ((m & airtime::kSrcRds) && (m & (airtime::kSrcWwv | airtime::kSrcManual))) ? "+" : "",
                 (m & airtime::kSrcWwv) ? "WWV" : ((m & airtime::kSrcManual) ? "SET" : ""));

  char age[16] = "";
  if(st.ever_synced)
  {
    const int64_t a = st.since_sync_us / 1000000;
    if(a < 90)          snprintf(age, sizeof(age), "%llds ago", (long long)a);
    else if(a < 5400)   snprintf(age, sizeof(age), "%lldm ago", (long long)(a / 60));
    else                snprintf(age, sizeof(age), "%lldh ago", (long long)(a / 3600));
  }

  if(!st.clock_valid)
    snprintf(statusBuf, sizeof(statusBuf), "ACQUIRING - no time yet");
  else if(!st.synced)
    snprintf(statusBuf, sizeof(statusBuf), "UNSYNCED - last known %s", age);
  else
    snprintf(statusBuf, sizeof(statusBuf), "%s   %s   sync %s", unc, src, age);

  // The honest dial, replacing a frequency readout this build cannot keep
  // truthful. Without it the screen cannot explain why the radio is playing
  // music (an FM station being harvested for RDS clock time, ~95% of the hour)
  // or why the audio has become a 1000 Hz beep.
  //
  // Until the clock is trusted, the same line also says WHY the radio is
  // there: it is hunting. Field request, during a stubborn UNSYNCED: "some
  // kind of search indicator... to indicate where it's searching and the fact
  // that it is searching". The frequency was already on screen; the missing
  // word was SEARCHING — an amber screen that names its target reads as a
  // process, where one that just sits there reads as a fault.
  // Short on purpose: this row shares its line with the NTP-clients string
  // drawn from the right, and the first cut of the SEARCHING prefix grew the
  // text until the two overprinted — photographed in the field as
  // "for RDSTAm0 clients". Fits in 21 characters or it collides.
  if(atApp->directive().wwv_listening)
    snprintf(tunedBuf, sizeof(tunedBuf), "%sWWV %ld kHz",
             st.synced ? "" : "SEARCH ",
             (long)atApp->directive().wwv_band_khz);
  else
    snprintf(tunedBuf, sizeof(tunedBuf), "%sFM %.1f RDS",
             st.synced ? "" : "SEARCH ",
             (double)atRds.tunedKhz() / 100.0);

  snprintf(clientsBuf, sizeof(clientsBuf), "NTP: %d client%s",
           st.ntp_clients, st.ntp_clients == 1 ? "" : "s");

  // ── Why it is still searching ─────────────────────────────────────────────
  // Only while unsynced: once time is good this row is noise, and the net
  // line wants the space. Three numbers, chosen because between them they
  // separate every way the RDS path fails:
  //
  //   RSSI/SNR   is there anything on the frequency at all? Below ~10 dBuV
  //              there is nothing to decode and no firmware change will help.
  //   the stage  which refusal dominates. not-FM means the chip is not where
  //              we think it is; no-sync means the decoder never locked on a
  //              signal we do have; empty means it locked and the FIFO is
  //              simply not producing.
  //   g          groups actually accepted. Non-zero with no sync means RDS
  //              works and the fault is downstream, in the CT decode.
  diagBuf[0] = 0;
  if(!st.synced)
  {
    if(atApp->directive().wwv_listening)
    {
      // During a listen window the RDS numbers describe a chip that is not on
      // FM — the story that matters is the WWV chain, stage by stage: pulses
      // measured, frames seen/read/paired, minute markers. A photograph of
      // this row says exactly how far up the ladder the band got.
      const airtime::WwvTimecodeDecoder& tc = atApp->wwvTimecode();
      snprintf(diagBuf, sizeof(diagBuf), "R%d p%lu f%lu/%lu/%lu mk%lu",
               (int)rssi,
               (unsigned long)atApp->wwvTimecodeDiag().pulses,
               (unsigned long)tc.framesSeen(),
               (unsigned long)tc.framesDecoded(),
               (unsigned long)tc.framesConfirmed(),
               (unsigned long)atApp->wwvMarker().diag().markers);
    }
    else
    {
      const uint32_t nf = atRds.pollsNotFm();
      const uint32_t ns = atRds.pollsNoSync();
      const uint32_t mt = atRds.pollsEmpty();
      const char *stage = "not-FM";
      uint32_t worst = nf;
      if(ns > worst) { worst = ns; stage = "no-sync"; }
      if(mt > worst) { worst = mt; stage = "empty";   }
      if(worst == 0) stage = "idle";
      snprintf(diagBuf, sizeof(diagBuf), "R%d S%d %s%lu g%lu t%lu f%lu",
               (int)rssi, (int)snr, stage, (unsigned long)worst,
               (unsigned long)atRds.groupsAccepted(),
               (unsigned long)atFmTunes, (unsigned long)atRdsSyncFound);
    }
  }

  // ── The cycle instrument ──────────────────────────────────────────────────
  // Off, or unsynchronised, it draws nothing. Two reasons, and the second one
  // cost a debugging round:
  //
  //  * A slot boundary computed from a clock we do not believe is worse than
  //    no boundary, because it looks exactly as authoritative as a correct one.
  //  * It shares the bottom row with the diagnostic line — the RSSI/SNR/stage
  //    readout that exists to explain WHY the radio is not syncing. Gating on
  //    clock_valid rather than synced meant that the moment an operator turned
  //    the instrument on, the one row that could answer "why is it unsynced"
  //    was covered by an instrument that was itself meaningless.
  //
  // So: the instrument appears when the clock is trustworthy, and the
  // diagnosis appears when it is not. They are never both wanted at once.
  cycleBuf[0] = cycleTBuf[0] = 0;
  out->cycle_fraction = 0.0f;
  out->cycle_odd = false;
  if(atCycleOpt > 0 && st.synced)
  {
    const airtime::CycleMode &m = airtime::kCycleModes[atCycleOpt - 1];
    const airtime::CyclePhase ph = airtime::cyclePhaseAt(st.utc_us, m.period_us);

    // Period as the operator quotes it: "15s", "7.5s", "120s".
    const long ms = (long)(m.period_us / 1000);
    if(ms % 1000)
      snprintf(cycleBuf, sizeof(cycleBuf), "%s %ld.%lds", m.name, ms / 1000,
               (ms % 1000) / 100);
    else
      snprintf(cycleBuf, sizeof(cycleBuf), "%s %lds", m.name, ms / 1000);

    // Two decimals under ten seconds — that is the range where an operator is
    // watching for the boundary and where our millisecond accuracy is the
    // point. One decimal above, so a WSPR countdown does not jitter uselessly.
    const double rem = (double)ph.remain_us / 1e6;
    snprintf(cycleTBuf, sizeof(cycleTBuf), rem < 10.0 ? "T-%.2fs" : "T-%.1fs", rem);

    out->cycle_fraction = ph.fraction;
    out->cycle_odd = ph.odd_slot;
  }

  // A survey owns the dial for half an hour. Saying so is the difference
  // between "working" and "broken" from the operator's side.
  if(atApp->surveying())
  {
    const airtime::FmSurvey& sv = atApp->survey();
    snprintf(netBuf, sizeof(netBuf), "SURVEY %d%%  %ld  (%u found)",
             sv.progressPct(), (long)sv.wantTuned(),
             (unsigned)sv.candidateCount());
    out->clock  = clockBuf; out->local = localBuf; out->zone = zoneBuf;
    out->status = statusBuf;
    out->tuned  = tunedBuf; out->clients = clientsBuf; out->net = netBuf;
    out->diag   = diagBuf;
    out->cycle = cycleBuf; out->cycle_t = cycleTBuf;
    out->cycle_fraction = 0.0f; out->cycle_odd = false;
    out->synced = st.synced; out->valid = st.clock_valid;
    return;
  }

  // What is on the air. Only ever shown with a clock we trust — a net schedule
  // read off a wrong clock is worse than no schedule, because it looks right.
  netBuf[0] = 0;
  if(st.clock_valid && st.synced)
  {
    const int64_t utc_s = st.utc_us / 1000000;
    const int active = airtime::netActiveAt(kNets, kNetCount, utc_s);
    if(active >= 0)
    {
      snprintf(netBuf, sizeof(netBuf), "NOW %s %ld", kNets[active].name,
               (long)kNets[active].khz);
    }
    else
    {
      int wait = 0;
      const int next = airtime::netNextAt(kNets, kNetCount, utc_s, &wait);
      if(next >= 0 && wait < 24 * 60)   // beyond a day it is not news
        snprintf(netBuf, sizeof(netBuf), "%s in %dh%02d", kNets[next].name,
                 wait / 60, wait % 60);
    }
  }

  out->clock  = clockBuf;
  out->local  = localBuf;
  out->zone   = zoneBuf;
  out->status = statusBuf;
  out->tuned  = tunedBuf;
  out->clients = clientsBuf;
  out->net    = netBuf;
  out->diag   = diagBuf;
  out->cycle  = cycleBuf;
  out->cycle_t = cycleTBuf;
  out->synced = st.synced;
  out->valid  = st.clock_valid;
}

// The serial console keeps the richer UTF-8 formatting from the core module.
void airtimeStatusLines(const char **l1, const char **l2)
{
  *l1 = nullptr;   // no override: let the AirTime layout draw its own screen
  *l2 = nullptr;
}

void airtimeLoop()
{
  if(atApp == nullptr) return;

  atApp->loop();
  atWifi.service(*atApp);

  // Feed the stock clock from the disciplined one. Everything stock keys off
  // clockGetHM() — most usefully the EiBi schedule lookups, which are UTC
  // schedules on a radio that, uniquely, actually knows UTC — was dead in
  // this build, because the paths that set the stock clock (NTP client, raw
  // RDS CT) are exactly the ones AirTime replaced.
  //
  // Fed as UTC: clockGetHM() hands back the raw stored value (what EiBi
  // needs), while the stock "Time:" display applies the operator's UTC-offset
  // setting on top — both consumers get the frame they expect. clockSet() is
  // set-once and free-runs on micros() thereafter, so a re-feed means
  // clockReset() first; every 10 minutes bounds the stock clock's drift at
  // ~20 ms, three orders of magnitude inside schedule resolution.
  {
    static uint32_t lastClockFeed = 0;
    const uint32_t feedNow = millis();
    if(!clockAvailable() || feedNow - lastClockFeed >= 600000)
    {
      const airtime::DisplayState cst = atApp->displayState();
      if(cst.clock_valid)
      {
        const int64_t sod = (cst.utc_us / 1000000) % 86400;
        clockReset();
        clockSet((uint8_t)(sod / 3600), (uint8_t)((sod / 60) % 60),
                 (uint8_t)(sod % 60));
        lastClockFeed = feedNow;
      }
    }
  }

  // The status page, after NTP and never before it. A client that opens a
  // socket and then says nothing can stall handleClient(); serving time is the
  // job and looking at diagnostics is not, so time goes first. (Nothing here
  // can disturb a WWV measurement: the AP is only ever up while the sampler is
  // stopped — PLAN.md §2.)
  airtimeWebService(atWifi.isUp());

  // The IO11 tap level follows the DSP volume, so a listen window has to hold
  // the level Milestone 0 calibrated — and then give the user their volume
  // back. atUserVolume remembers what they had.
  //
  // The knob still works during a window (the stock menu is live in this build
  // and rx.setVolume() is applied immediately by doVolume). Left alone, that
  // would quietly de-calibrate the marker detector mid-measurement. So a turn
  // during a window is honoured as INTENT — remembered for when the window
  // closes — while the tap stays where the detector needs it.
  const bool listening = atApp->directive().wwv_listening;
  if(listening != atWasListening)
  {
    if(listening) { atUserVolume = volume; rx.setVolume(kWwvListenVolume); }
    else          { volume = atUserVolume; rx.setVolume(volume); }
    atWasListening = listening;
  }
  else if(listening && volume != atUserVolume)
  {
    atUserVolume = volume;            // they turned it; apply it afterwards
    rx.setVolume(kWwvListenVolume);   // ...but not to the tap, not right now
  }

  const uint32_t nowMs = millis();
  if(nowMs - atLastReport >= 10000)
  {
    atLastReport = nowMs;
    char l1[32], l2[96];
    const airtime::DisplayState st = atApp->displayState();
    airtime::formatUtcLine(st, l1, sizeof(l1));
    airtime::formatStatusLine(st, l2, sizeof(l2));
    Serial.printf("AirTime %s | %s\n", l1, l2);
    const airtime::WwvMarkerDiag md = atApp->wwvMarker().diag();
    // ap[] tells the truth about the radio, not our intent: DOWN* means the
    // directive wants the AP up but esp_wifi refused (it retries every 500 ms
    // and afail counts the refusals). Field-earned: the AP was once silently
    // dead for most of an hour while this line looked healthy.
    const bool apWanted = atApp->directive().wifi_up;
    Serial.printf(
        "  wwv[fs=%.0f dc=%.0f blk=%lu drop=%lu] rds[ok=%lu rej=%lu] "
        "ap[%s %d joined, %lu served, afail=%lu]\n",
        (double)atWwv.sampleRateHz(), (double)atWwv.dcLevel(),
        (unsigned long)atWwv.blocksProduced(), (unsigned long)atWwv.blocksDropped(),
        (unsigned long)atRds.groupsAccepted(), (unsigned long)atRds.groupsRejected(),
        atWifi.isUp() ? "up" : (apWanted ? "DOWN*" : "down"),
        atWifi.stationCount(), (unsigned long)atWifi.requestsServed(),
        (unsigned long)atWifi.upFailures());
    // The marker detector's view — the number that decides the next move.
    // flr/pk: current noise floor and strongest block seen (normalised power).
    // st: bursts that crossed the on-threshold; run: last/longest burst ms;
    // rej: duration-gate rejects short/long; mk: accepted minute markers.
    // band= is the WWV frequency currently being listened to (0 = not in a
    // window). Diag counters are cumulative; attribute deltas to the band
    // shown while they moved. Field report: music heard on ~10 MHz at this
    // QTH — a jammed band inflates flr and buries the beep, so per-band
    // attribution is the whole game.
    Serial.printf(
        "  mkr[band=%ld flr=%.1e pk=%.1e st=%lu run=%ld/%ldms rej=%lu/%lu mk=%lu]\n",
        (long)atApp->directive().wwv_band_khz,
        (double)md.noise_floor, (double)md.max_power,
        (unsigned long)md.tone_starts,
        (long)(md.last_tone_us / 1000), (long)(md.longest_tone_us / 1000),
        (unsigned long)md.rejected_short, (unsigned long)md.rejected_long,
        (unsigned long)md.markers);
    // The 100 Hz code chain, same shape: the subcarrier bin's health, then
    // each stage of the ladder. blk/drop are the second queue; flr/pk the
    // pulse detector's view of the 20 Hz bin — the numbers that will either
    // confirm or correct the provisional thresholds in AppConfig.
    const airtime::WwvMarkerDiag pdg = atApp->wwvPulse().diag();
    const airtime::WwvTimecodeDecoder& tcd = atApp->wwvTimecode();
    const airtime::WwvTimecodeDiag& tdg = atApp->wwvTimecodeDiag();
    Serial.printf(
        "  code[blk=%lu drop=%lu flr=%.1e pk=%.1e pulses=%lu spl=%lu gap=%lu "
        "frames=%lu read=%lu ok=%lu]\n",
        (unsigned long)atWwv.subBlocksProduced(),
        (unsigned long)atWwv.subBlocksDropped(),
        (double)pdg.noise_floor, (double)pdg.max_power,
        (unsigned long)tdg.pulses, (unsigned long)tdg.splinters,
        (unsigned long)tdg.gap_seconds, (unsigned long)tcd.framesSeen(),
        (unsigned long)tcd.framesDecoded(),
        (unsigned long)tcd.framesConfirmed());
    // Every NEW complete frame goes out raw — sixty symbols beside whatever
    // UTC the reader trusts. This line, photographed or logged next to a
    // known clock, is the empirical check of the bit map that the decoder
    // header promises: one capture and the table is confirmed or corrected
    // without guesswork. 0/1/M/? = zero, one, position marker, unreadable.
    static uint32_t lastFramesSeen = 0;
    if(tcd.framesSeen() != lastFramesSeen)
    {
      lastFramesSeen = tcd.framesSeen();
      char sym[61];
      const airtime::TcSymbol *raw = tcd.rawFrame();
      for(int i = 0 ; i < 60 ; i++)
        sym[i] = raw[i] == airtime::TcSymbol::Zero   ? '0' :
                 raw[i] == airtime::TcSymbol::One    ? '1' :
                 raw[i] == airtime::TcSymbol::Marker ? 'M' : '?';
      sym[60] = 0;
      Serial.printf("  frame[%s]\n", sym);
      if(tcd.haveFrame())
      {
        const airtime::WwvTime& wt = tcd.time();
        Serial.printf("  frame-> doy %d %02d:%02d UTC %d dut1=%+dms%s%s\n",
                      wt.day_of_year, wt.hour, wt.minute, wt.year, wt.dut1_ms,
                      wt.dst_now ? " DST" : "", wt.leap_warning ? " LEAP" : "");
      }
    }
    // What the arbiter DID with what it was told — the half of the story the
    // source counters cannot tell. Both failures that cost a session here were
    // invisible without this: three good WWV markers that were all rejected
    // (identical to "no markers" from the mkr line), and an accepted +3765 s
    // RDS correction that was never actually applied (identical to "the clock
    // is fine" from the status line).
    //   wwv off/pair/act : implied correction; agreed with the previous
    //                      minute's marker; Applied or Rejected-and-held.
    //   rds off/n/act    : consensus correction; agreeing stations; ditto.
    //   pend             : correction accepted but not yet slewed in. This is
    //                      known error on top of the ± figure above, and it
    //                      should fall to 0. If it sits there, the clock is
    //                      crawling at its slew ceiling and is NOT synced,
    //                      whatever the ± says.
    const airtime::WwvFixDiag fd = atApp->wwvFixDiag();
    const airtime::RdsFixDiag rd = atApp->rdsFixDiag();
    const int64_t pend =
        atApp->arbiter().pendingCorrectionUs(atMono.nowUs()) / 1000;
    Serial.printf("  fix[");
    if(fd.have)
      Serial.printf("wwv %+ldms pair=%c %c | ", (long)(fd.offset_us / 1000),
                    fd.corroborated ? 'Y' : 'n', fd.accepted ? 'A' : 'R');
    if(rd.have)
      Serial.printf("rds %+ldms n=%d %c | ", (long)(rd.offset_us / 1000),
                    rd.stations, rd.accepted ? 'A' : 'R');
    Serial.printf("pend=%ldms] rate=%+.1fppm\n", (long)pend,
                  atApp->arbiter().ratePpm());

    // What WWV has taught about each station: PI code, how late it runs, and
    // how many times that has been measured. n reaching 3 is the moment a
    // station's correction starts being applied — and the moment a badly-late
    // station stops being rejected on sight and becomes useful again.
    const airtime::StationBiasTable& sb = atApp->stationBias();
    if(sb.count() > 0)
    {
      Serial.printf("  sta[");
      for(size_t i = 0 ; i < sb.count() ; i++)
      {
        const airtime::StationBias& b = sb.at(i);
        Serial.printf("%s%04X %+ldms n=%d", i ? " | " : "", b.pi,
                      (long)(b.bias_us / 1000), b.samples);
      }
      Serial.printf("]\n");
    }
  }
}

#endif  // AIRTIME
