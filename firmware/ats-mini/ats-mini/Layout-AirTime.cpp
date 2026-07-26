//
// The AirTime screen — PLAN.md §5.
//
// This build is a clock appliance, not a receiver UI, and the screen should say
// so. The stock layouts lead with the frequency readout, which in this build is
// actively false: AirTime retunes the chip continuously and `currentFrequency`
// never hears about it, so the dial shows whatever was there last. That is not
// a cosmetic problem — a leftover "9999" from a probe build once sent the owner
// hunting a jamming theory that did not exist, and the same reading later hid
// the fact that the radio was quietly parked on an FM music station.
//
// So: the time is the headline, in the 48-pixel seven-segment font (TFT_eSPI
// font 7, whose entire character set is "1234567890:-." — it exists to draw
// clocks). Underneath it, the two things a time appliance must never hide:
// how much it can be trusted, and what the radio is actually doing right now.
//
// ── ASCII only ──────────────────────────────────────────────────────────────
// The core's display module formats for a terminal and uses "±" and "·". The
// TFT fonts are ASCII, so those render as gaps — visible in the first photo
// from the device as "250 ms  RDS  sync 26s ago". The strings here are built
// separately, in ASCII, rather than reusing the serial ones.

#ifdef AIRTIME

#include "Common.h"
#include "Themes.h"
#include "Draw.h"
#include "Utils.h"

// Supplied by AirTimeMode.cpp — the app's state, pre-formatted for this screen.
struct AirTimeScreen {
  const char *clock;    // "23:31:26", or "--:--:--" before any fix
  const char *status;   // "+/-250 ms   RDS   sync 26s ago"
  const char *tuned;    // "FM 89.9 MHz" / "WWV 15000 kHz  LISTENING"
  const char *clients;  // "NTP: 2 clients"
  bool synced;          // false => clock drawn in the warning colour
  bool valid;           // false => no time at all yet
};
extern void airtimeScreen(AirTimeScreen *out);

void drawLayoutAirTime(const char *statusLine1, const char *statusLine2)
{
  // statusLine1/2 are the caller's override (menus, BLE, EiBi). When present
  // they win: a transient message the operator asked for should not be buried
  // under the clock.
  const bool override_status = statusLine1 || statusLine2;

  AirTimeScreen s;
  airtimeScreen(&s);

  drawSaveIndicator(SAVE_OFFSET_X, SAVE_OFFSET_Y);
  drawBleIndicator(BLE_OFFSET_X, BLE_OFFSET_Y);
  const bool has_voltage = drawBattery(BATT_OFFSET_X, BATT_OFFSET_Y);
  drawWiFiIndicator(has_voltage ? WIFI_OFFSET_X : BATT_OFFSET_X - 13, WIFI_OFFSET_Y);

  // "AirTime" wordmark, top left, where the band name sits in the stock layout.
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString("AirTime", 8, 6, 2);

  // ── The clock ─────────────────────────────────────────────────────────────
  // Font 7 is 48 px tall and about 32 px per character, so "23:31:26" spans
  // roughly 250 px of the 320 px panel — centred, with room either side.
  // Amber rather than green while unsynchronised: §5 requires the device to
  // look different when it is coasting, not merely to say so in small print.
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(s.synced ? TH.text : TH.text_warn);
  spr.drawString(s.clock, 160, 34, 7);

  // "UTC" tucked under the seconds — font 7 has no letters at all.
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString("UTC", 300, 86, 2);

  // ── What it can be trusted to, and what the radio is doing ────────────────
  spr.setTextDatum(TC_DATUM);
  if(override_status)
  {
    spr.setTextColor(TH.rds_text);
    if(statusLine1) spr.drawString(statusLine1, 160, 112, 2);
    if(statusLine2) spr.drawString(statusLine2, 160, 129, 2);
  }
  else
  {
    spr.setTextColor(s.valid ? TH.text : TH.text_warn);
    spr.drawString(s.status, 160, 112, 2);

    // The honest dial. Without this line the screen cannot explain why the
    // radio is playing music (it is on an FM station, harvesting RDS clock
    // time — 95% of every hour) or why the audio just became a beep.
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(TH.text_muted);
    spr.drawString(s.tuned, 8, 133, 2);

    spr.setTextDatum(TR_DATUM);
    spr.drawString(s.clients, 312, 133, 2);
  }

  // Signal strength stays: it is the one stock reading still true here, and it
  // tells you at a glance whether the station being harvested is receivable.
  drawSMeter(getStrength(rssi), METER_OFFSET_X, METER_OFFSET_Y);
}

#endif  // AIRTIME
