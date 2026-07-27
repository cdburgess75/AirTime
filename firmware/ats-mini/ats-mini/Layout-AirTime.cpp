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
#include "Menu.h"
#include "Utils.h"

// Supplied by AirTimeMode.cpp — the app's state, pre-formatted for this screen.
struct AirTimeScreen {
  const char *clock;    // "23:31:26", or "--:--:--" before any fix
  const char *local;    // same instant in the operator's zone
  const char *zone;     // "CDT" — see kLocalZoneLabel in AirTimeMode.cpp
  const char *status;   // "+/-250 ms   RDS   sync 26s ago"
  const char *tuned;    // "FM 89.9 MHz" / "WWV 15000 kHz  LISTENING"
  const char *clients;  // "NTP: 2 clients"
  const char *net;      // what is on the air, or "" 
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

  // A menu needs the left half of the panel, and the 48 px clock starts at
  // x=32 — they were drawing straight through each other, sidebar over digits.
  // While a menu is open the time steps aside: smaller, top right, still
  // readable, out of the way. (Seen on the device: the Nets box landed on top
  // of "5:47:14" and the zoom overlay ran off the right edge.)
  const bool menu_open = (currentCmd != CMD_NONE);

  AirTimeScreen s;
  airtimeScreen(&s);

  drawSaveIndicator(SAVE_OFFSET_X, SAVE_OFFSET_Y);
  drawBleIndicator(BLE_OFFSET_X, BLE_OFFSET_Y);
  const bool has_voltage = drawBattery(BATT_OFFSET_X, BATT_OFFSET_Y);
  drawWiFiIndicator(has_voltage ? WIFI_OFFSET_X : BATT_OFFSET_X - 13, WIFI_OFFSET_Y);

  // Wordmark. Sits in the gap between the S-meter (drawn from x=0) and the
  // WiFi icon at x=237, which is where the stock layout puts the band name.
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString("AirTime", 150, 3, 2);

  // ── The two clocks ────────────────────────────────────────────────────────
  // LOCAL time is the headline, in font 7 — a 48-pixel seven-segment face whose
  // whole character set is "1234567890:-.", i.e. a clock font. UTC sits under
  // it in font 4 (26 px) and a different colour.
  //
  // The radio keeps and serves UTC and always will; this is purely about who
  // is reading the screen. Someone glancing at a clock on the bench wants the
  // time on their wrist. UTC still has to be present and unambiguous — it is
  // what NTP carries and what a log entry needs — but it does not have to
  // shout, and the "Z" makes it unmistakable at a glance.
  //
  // Both are right-aligned to the same edge so the two labels stack in a tidy
  // column. Amber instead of white while unsynchronised — §5 wants the device
  // to LOOK wrong when it is coasting, not to explain itself in small print.
  const uint16_t clock_colour = s.synced ? TH.text : TH.text_warn;

  if(menu_open)
  {
    // Compact: time and zone on the right, clear of the side bar entirely.
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(clock_colour);
    spr.drawString(s.valid ? s.local : s.clock, 312, 22, 4);
    spr.setTextColor(TH.text_muted);
    spr.drawString(s.valid ? s.zone : "UTC", 312, 48, 2);
    spr.setTextColor(TH.smeter_bar);
    spr.drawString(s.clock, 312, 66, 2);

    // Whatever the open menu wants to say in full, in the space the side bar
    // leaves free. The nets list shows names only; this is where the selected
    // one gets its frequency and its timing.
    if(currentCmd == CMD_AT_NETS)
    {
      spr.setTextDatum(BR_DATUM);
      spr.setTextColor(TH.text);
      spr.drawString(atNetDetail(), 312, 160, 2);
    }
    drawSMeter(getStrength(rssi), METER_OFFSET_X, METER_OFFSET_Y);
    drawSideBar(currentCmd, MENU_OFFSET_X, MENU_OFFSET_Y, MENU_DELTA_X);
    return;
  }

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(clock_colour);
  spr.drawString(s.valid ? s.local : s.clock, 248, 20, 7);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.text_muted);
  spr.drawString(s.valid ? s.zone : "UTC", 254, 50, 2);

  if(s.valid)
  {
    // The theme's meter green: every theme keeps it a legible accent, and it
    // reads as clearly "the other number" at a glance.
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(TH.smeter_bar);
    spr.drawString(s.clock, 248, 74, 4);

    spr.setTextDatum(TL_DATUM);
    spr.drawString("UTC", 254, 80, 2);
  }

  // ── What it can be trusted to, and what the radio is doing ────────────────
  if(override_status)
  {
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(TH.rds_text);
    if(statusLine1) spr.drawString(statusLine1, 160, 108, 2);
    if(statusLine2) spr.drawString(statusLine2, 160, 125, 2);
  }
  else
  {
    spr.setTextDatum(TC_DATUM);
    spr.setTextColor(s.valid && s.synced ? TH.text_muted : TH.text_warn);
    spr.drawString(s.status, 160, 108, 2);

    // The honest dial. Without this line the screen cannot explain why the
    // radio is playing music (it is on an FM station, harvesting RDS clock
    // time — 95% of every hour) or why the audio just became a beep.
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(TH.text_muted);
    spr.drawString(s.tuned, 8, 132, 2);

    spr.setTextDatum(TR_DATUM);
    spr.drawString(s.clients, 312, 132, 2);

    // What is on the air right now — the line only an accurate clock can
    // write. Shown in the accent colour so it reads as news, not status.
    if(s.net[0])
    {
      spr.setTextDatum(TC_DATUM);
      spr.setTextColor(TH.smeter_bar);
      spr.drawString(s.net, 160, 150, 2);
    }
  }

  // Signal strength stays: it is the one stock reading still true here, and it
  // tells you at a glance whether the station being harvested is receivable.
  drawSMeter(getStrength(rssi), METER_OFFSET_X, METER_OFFSET_Y);

  // The stock menu, drawn LAST so it sits over the clock.
  //
  // Every ats-mini control still works in this build — press the encoder and
  // turn for volume, bandwidth, AGC, theme, UTC offset. Dropping the side bar
  // when this layout replaced the stock one left those controls functional but
  // invisible, which is worse than removing them: the owner asked how to
  // change the volume while it was in fact already changing under his hand.
  // The bar appears only while a command is active, so the clock has the
  // screen to itself the rest of the time.
  if(currentCmd != CMD_NONE)
    drawSideBar(currentCmd, MENU_OFFSET_X, MENU_OFFSET_Y, MENU_DELTA_X);
}

#endif  // AIRTIME
