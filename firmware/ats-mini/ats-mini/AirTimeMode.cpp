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
#include "Menu.h"   // getCurrentUTCOffset(), utcOffsets[] — the device's own
                    // timezone setting, so local time is adjustable in the field

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
static int atHfOpt = 0;

// ── Nets worth knowing about ────────────────────────────────────────────────
// The feature the clock earns: a receiver that knows UTC to milliseconds can
// answer "is it on NOW", not merely "what frequency is it on".
//
// **VERIFY THESE AGAINST THE CURRENT PUBLISHED SCHEDULE BEFORE RELYING ON
// THEM.** Net times and frequencies drift, move seasonally, and are changed by
// their controls; W1AW's schedule in particular shifts with US daylight time
// while these entries are fixed UTC. They are here as a working starting point
// and an example of the format — edit freely, the table is plain data and the
// logic that reads it is in airtime/nets.h.
//
// Times are UTC minutes past midnight, days are a UTC weekday mask.
static const airtime::HamNet kNets[] = {
  // name              kHz    mode                days       start        mins
  {"Maritime Mobile", 14300, airtime::NetMode::Usb, airtime::kDaily, 12*60,   600},
  {"40m Evening",      7200, airtime::NetMode::Lsb, airtime::kDaily,  1*60,   120},
  {"W1AW CW Prac",     7047, airtime::NetMode::Cw,  airtime::kWeekdays, 14*60,  60},
  {"W1AW Bulletin",    7047, airtime::NetMode::Cw,  airtime::kDaily,  0*60,    30},
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
  // stale status struct; the adapter relies on this <0 to never read it.
  if(!rx.isCurrentTuneFM()) return -1;
  rx.getRdsStatus(1, 0, 0);       // INTACK: pop one group, ack the flags
  if(!rx.getRdsSync()) return -1;
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

int atHfCount() { return (int)(sizeof(kHfNames) / sizeof(kHfNames[0])); }
const char *atHfName(int i) { return (i >= 0 && i < atHfCount()) ? kHfNames[i] : "?"; }
int atHfIdx() { return atHfOpt; }
void atSetHfIdx(int i)
{
  if(i < 0 || i >= atHfCount()) return;
  atHfOpt = i;
  if(atApp == nullptr) return;
  // Acted on as the operator scrolls: these are verbs, not a stored preference,
  // and §5 asks for them to be immediate.
  if(atHfOpt == 0)      atApp->operatorListenNow();
  else if(atHfOpt == 1)  atApp->operatorServeNow();
  else                   atApp->startSurvey();   // ~30 min; owns the dial
}

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

struct AirTimeScreen {
  const char *clock;
  const char *local;
  const char *zone;
  const char *status;
  const char *tuned;
  const char *clients;
  const char *net;      // "NOW: Maritime Mobile 14300" / "Next: ... in 2h10"
  bool synced;
  bool valid;
};

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
  if(atApp->directive().wwv_listening)
    snprintf(tunedBuf, sizeof(tunedBuf), "WWV %ld kHz  LISTENING",
             (long)atApp->directive().wwv_band_khz);
  else
    snprintf(tunedBuf, sizeof(tunedBuf), "FM %.1f MHz  RDS",
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
