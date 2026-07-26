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

static const uint8_t kWwvListenVolume = 35; // the level Milestone 0 calibrated

static airtime_esp32::EspMonotonicClock atMono;
static airtime_esp32::Esp32RdsSource atRds;
static airtime_esp32::Esp32WwvSampler atWwv;
static airtime_esp32::Esp32WiFiControl atWifi;
static airtime_esp32::NvsTimeStore atStore;
static airtime::AirTimeApp* atApp = nullptr;
static bool atWasListening = false;
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

  airtime_esp32::RdsSourceConfig rdsCfg;
  airtime_esp32::RdsChipOps rdsOps;
  rdsOps.tune = &atTuneFm;
  rdsOps.read = &atRdsRead;
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

  Serial.println("AirTime: up. NTP at 192.168.4.1:123 while serving.");
}

// The §5 display. ats-mini's drawScreen() already takes two status lines and
// AirTime's display module already produces exactly two, so the clock lands in
// the stock layout with no new drawing code and no fight with the existing UI.
//
// This matters more than it looks. In this build the frequency readout is
// actively misleading — the scheduler retunes the chip constantly, so whatever
// the dial says is a leftover. That cost a whole debugging session once: the
// display read "9999" from an earlier probe build and sent us hunting a
// jamming theory that did not exist. A time appliance should show the time and
// how much it can be trusted, and nothing it cannot stand behind.
//
// Buffers are static because drawScreen keeps the pointers only for the length
// of the call, but the caller reads them after we return.
void airtimeStatusLines(const char **l1, const char **l2)
{
  static char line1[32] = "AirTime";
  static char line2[96] = "starting...";
  if(atApp != nullptr)
  {
    const airtime::DisplayState st = atApp->displayState();
    airtime::formatUtcLine(st, line1, sizeof(line1));
    airtime::formatStatusLine(st, line2, sizeof(line2));
  }
  *l1 = line1;
  *l2 = line2;
}

void airtimeLoop()
{
  if(atApp == nullptr) return;

  atApp->loop();
  atWifi.service(*atApp);

  // The IO11 tap level follows the DSP volume; hold the calibrated level for
  // the whole listening window, then give the user their volume back.
  const bool listening = atApp->directive().wwv_listening;
  if(listening != atWasListening)
  {
    rx.setVolume(listening ? kWwvListenVolume : volume);
    atWasListening = listening;
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
  }
}

#endif  // AIRTIME
