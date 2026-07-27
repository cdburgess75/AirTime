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
static const int32_t kFmStations[] = {
    8990,   // 89.9  WWNO   pi=A920  +37..+87 ms (n=3) — the anchor
    10470,  // 104.7 WJSH   pi=6E47  +216 ms (n=1)
    10750,  // 107.5 K-LOVE pi=33CB  +352..+367 ms (n=2)
};
static const size_t kFmStationCount =
    sizeof(kFmStations) / sizeof(kFmStations[0]);

// WWV band order — a warm start like the FM list above. 15 MHz first: it is
// the only band that has produced a marker at this QTH (two sessions of logs),
// which matches §4's daytime expectation (10/15 by day, 5 at night). The
// scheduler's learned per-band preference takes over once anything delivers;
// this only decides where a fresh boot looks FIRST, so the first window is
// spent on the likeliest band instead of sweeping two dead ones.
static const int32_t kWwvBands[] = {15000, 10000, 5000};
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
static const char* const kBandNames[] = {"Auto", "15 MHz", "10 MHz", "5 MHz"};
static const int32_t kBandKhz[] = {0, 15000, 10000, 5000};
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

static void atTuneFm(int32_t khz10, void*)
{
  rx.setFM(6400, 10800, (uint16_t)khz10, 10);
  rx.setGpioCtl(1, 0, 0);
  rx.setGpio(0, 0, 0);            // FM antenna path
  rx.RdsInit();
  rx.setRdsConfig(1, 2, 2, 2, 2); // chip-level ceiling; adapter gates tighter
}

static void atTuneWwv(int32_t khz, void*)
{
  rx.setAM(150, 30000, (uint16_t)khz, 5);
  rx.setGpioCtl(1, 0, 0);
  rx.setGpio(1, 0, 0);            // whip/SW antenna path
  rx.setBandwidth(2, 1);          // 3 kHz — the 1000 Hz marker passes cleanly
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
  const uint8_t blob[3] = {1, (uint8_t)atZone, (uint8_t)atBandOpt};  // [version, ...]
  atStore.saveBlob("cfg", blob, sizeof(blob));
}

static void atLoadSettings()
{
  uint8_t blob[8];
  size_t n = 0;
  if(!atStore.loadBlob("cfg", blob, sizeof(blob), &n)) return;
  if(n < 3 || blob[0] != 1) return;            // unknown format: keep defaults
  if(blob[1] < kZoneCount)    atZone    = blob[1];
  if(blob[2] < kBandOptCount) atBandOpt = blob[2];
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

  if(atApp == nullptr)
  {
    out->clock = clockBuf; out->local = localBuf; out->zone = zoneBuf;
    out->status = statusBuf;
    out->tuned = tunedBuf; out->clients = clientsBuf; out->net = netBuf;
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
