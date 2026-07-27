#include "math.h"
#include "Common.h"
#include "Themes.h"
#include "Utils.h"
#include "Draw.h"
#include "EIBI.h"
#include "BleMode.h"
#include "Menu.h"

//
// Bands Menu
//
// TO CONFIGURE YOUR OWN BAND PLAN:
// Add new bands by inserting new lines in the table below. Remove
// bands by deleting lines. Change bands by editing lines below.
//
// NOTE:
// You have to RESET PREFERENCES after adding or removing lines in this
// table. Turn your receiver on with the encoder push button pressed
// at first time to RESET the preferences.
//

int bandIdx = 0;

// Band limits are expanded to align with the nearest tuning scale mark
// Do not forget to update the bands table in the manual.md
Band bands[] =
{
  {"VHF",  FM_BAND_TYPE, FM,   6400, 10800, 10390, 2, 0, 0, 0},
  // All band. LW, MW and SW (from 150kHz to 30MHz)
  {"ALL",  SW_BAND_TYPE, AM,    150, 30000, 15000, 1, 4, 0, 0},
  {"11M",  SW_BAND_TYPE, AM,  25600, 26100, 25850, 1, 4, 0, 0},
  {"13M",  SW_BAND_TYPE, AM,  21500, 21900, 21650, 1, 4, 0, 0},
  {"15M",  SW_BAND_TYPE, AM,  18900, 19100, 18950, 1, 4, 0, 0},
  {"16M",  SW_BAND_TYPE, AM,  17400, 18100, 17650, 1, 4, 0, 0},
  {"19M",  SW_BAND_TYPE, AM,  15100, 15900, 15450, 1, 4, 0, 0},
  {"22M",  SW_BAND_TYPE, AM,  13500, 13900, 13650, 1, 4, 0, 0},
  {"25M",  SW_BAND_TYPE, AM,  11000, 13000, 11850, 1, 4, 0, 0},
  {"31M",  SW_BAND_TYPE, AM,   9000, 11000,  9650, 1, 4, 0, 0},
  {"41M",  SW_BAND_TYPE, AM,   7000,  9000,  7300, 1, 4, 0, 0},
  {"49M",  SW_BAND_TYPE, AM,   5000,  7000,  6000, 1, 4, 0, 0},
  {"60M",  SW_BAND_TYPE, AM,   4000,  5100,  4950, 1, 4, 0, 0},
  {"75M",  SW_BAND_TYPE, AM,   3500,  4000,  3950, 1, 4, 0, 0},
  {"90M",  SW_BAND_TYPE, AM,   3000,  3500,  3300, 1, 4, 0, 0},
//  {"25M",  SW_BAND_TYPE, AM,  11600, 12100, 11850, 1, 4, 0},
//  {"31M",  SW_BAND_TYPE, AM,   9400,  9900,  9650, 1, 4, 0},
//  {"41M",  SW_BAND_TYPE, AM,   7200,  7500,  7300, 1, 4, 0},
//  {"49M",  SW_BAND_TYPE, AM,   5900,  6200,  6000, 1, 4, 0},
//  {"60M",  SW_BAND_TYPE, AM,   4700,  5100,  4950, 1, 4, 0},
//  {"75M",  SW_BAND_TYPE, AM,   3900,  4000,  3950, 1, 4, 0},
//  {"90M",  SW_BAND_TYPE, AM,   3200,  3400,  3300, 1, 4, 0},
  {"MW3",  MW_BAND_TYPE, AM,   1700,  3500,  2500, 1, 4, 0, 0},
  {"MW2",  MW_BAND_TYPE, AM,    495,  1701,   783, 2, 4, 0, 0},
  {"MW1",  MW_BAND_TYPE, AM,    150,  1800,   810, 3, 4, 0, 0},
  {"160M", MW_BAND_TYPE, LSB,  1800,  2000,  1900, 5, 4, 0, 0},
  {"80M",  SW_BAND_TYPE, LSB,  3500,  4000,  3800, 5, 4, 0, 0},
  {"40M",  SW_BAND_TYPE, LSB,  7000,  7300,  7150, 5, 4, 0, 0},
  {"30M",  SW_BAND_TYPE, LSB, 10000, 10200, 10125, 5, 4, 0, 0},
  {"20M",  SW_BAND_TYPE, USB, 14000, 14400, 14100, 5, 4, 0, 0},
  {"17M",  SW_BAND_TYPE, USB, 18000, 18200, 18115, 5, 4, 0, 0},
  {"15M",  SW_BAND_TYPE, USB, 21000, 21500, 21225, 5, 4, 0, 0},
  {"12M",  SW_BAND_TYPE, USB, 24800, 25000, 24940, 5, 4, 0, 0},
  {"10M",  SW_BAND_TYPE, USB, 28000, 29700, 28500, 5, 4, 0, 0},
  // https://www.hfunderground.com/wiki/CB
  // Also see MIN_CB_FREQUENCY and MAX_CB_FREQUENCY
  {"CB",   SW_BAND_TYPE, AM,  25000, 28000, 27135, 0, 4, 0, 0},
};

int getTotalBands() { return(ITEM_COUNT(bands)); }
Band *getCurrentBand() { return(&bands[bandIdx]); }

//
// Main Menu
//

#ifdef AIRTIME
// The root menu carries only what this device is FOR. Everything that tunes a
// receiver is still present and unchanged -- it has simply moved under
// Settings, where configuration belongs. A clock appliance whose front page is
// twelve radio controls is a radio with a clock bolted on.
// Volume leads, and is where the menu opens on a cold boot. It is the one
// control an operator reaches for without planning to, and burying it under
// Settings made the most-used knob the deepest one.
#define MENU_VOLUME       0
#define MENU_MODE_AT      1
#define MENU_NETS         2
#define MENU_SETTINGS     3
#else
#define MENU_MODE         0
#define MENU_BAND         1
#define MENU_VOLUME       2
#define MENU_STEP         3
#define MENU_SEEK         4
#define MENU_SCAN         5
#define MENU_MEMORY       6
#define MENU_SQUELCH      7
#define MENU_BW           8
#define MENU_AGC_ATT      9
#define MENU_AVC         10
#define MENU_SOFTMUTE    11
#define MENU_SETTINGS    12
#endif

#ifdef AIRTIME
int8_t menuIdx = MENU_VOLUME;
#else
int8_t menuIdx = MENU_VOLUME;
#endif

static const char *menu[] =
{
#ifdef AIRTIME
  "Volume",
  "Mode",
  "Nets",
#else
  "Mode",
  "Band",
  "Volume",
  "Step",
  "Seek",
  "Scan",
  "Memory",
  "Squelch",
  "Bandwidth",
  "AGC/ATTN",
  "AVC",
  "SoftMute",
#endif
  "Settings",
};

// The index defines above and this array are two halves of one thing, and
// nothing in C makes them agree. They already disagreed once: a patch that
// looked for "Soft Mute" found "SoftMute", so the defines moved and the array
// did not. MENU_MODE_AT then pointed at "Settings", and choosing Settings
// opened the Clock/Radio list -- a bug that compiled, ran, and simply lied
// about where things were. Make the mismatch a build error instead.
static_assert(ITEM_COUNT(menu) == MENU_SETTINGS + 1,
              "menu[] and the MENU_* indices disagree");

//
// Settings Menu
//

#ifdef AIRTIME
// Relocated from the root menu. Same commands, same handlers, same order --
// only the door they are behind has changed.
#define MENU_MODE         0
#define MENU_BAND         1
#define MENU_STEP         2
#define MENU_SEEK         3
#define MENU_SCAN         4
#define MENU_MEMORY       5
#define MENU_SQUELCH      6
#define MENU_BW           7
#define MENU_AGC_ATT      8
#define MENU_AVC          9
#define MENU_SOFTMUTE    10
#define MENU_BRIGHTNESS  11
#define AT_SETTINGS_BASE 11
#else
#define MENU_BRIGHTNESS   0
#define AT_SETTINGS_BASE  0
#endif
// Every one of these is offset by AT_SETTINGS_BASE, which is zero in the stock
// build and twelve in AirTime's -- where the twelve relocated radio controls sit
// ahead of them in settings[]. Leaving the raw literals here is not a cosmetic
// slip: MENU_CALIBRATION(1) would collide with MENU_BAND(1), MENU_RDS(2) with
// MENU_VOLUME(2), and so on down the list, which the compiler catches only
// because clickSettings() switches on them (duplicate case value).
#define MENU_CALIBRATION (AT_SETTINGS_BASE + 1)
#define MENU_RDS         (AT_SETTINGS_BASE + 2)
#define MENU_UTCOFFSET   (AT_SETTINGS_BASE + 3)
#define MENU_FM_REGION   (AT_SETTINGS_BASE + 4)
#define MENU_THEME       (AT_SETTINGS_BASE + 5)
#define MENU_UI          (AT_SETTINGS_BASE + 6)
#define MENU_ZOOM        (AT_SETTINGS_BASE + 7)
#define MENU_SCROLL      (AT_SETTINGS_BASE + 8)
#define MENU_SLEEP       (AT_SETTINGS_BASE + 9)
#define MENU_SLEEPMODE   (AT_SETTINGS_BASE + 10)
#define MENU_LOADEIBI    (AT_SETTINGS_BASE + 11)
#define MENU_USBMODE     (AT_SETTINGS_BASE + 12)
#define MENU_BLEMODE     (AT_SETTINGS_BASE + 13)
#define MENU_WIFIMODE    (AT_SETTINGS_BASE + 14)
#ifdef AIRTIME
#define MENU_AT_ZONE     (AT_SETTINGS_BASE + 15)
#define MENU_AT_BAND     (AT_SETTINGS_BASE + 16)
#define MENU_AT_HF       (AT_SETTINGS_BASE + 17)
#define MENU_ABOUT       (AT_SETTINGS_BASE + 18)
#else
#define MENU_ABOUT       (AT_SETTINGS_BASE + 15)
#endif


#ifdef AIRTIME
// Top of the list, which is now the relocated radio controls. Leaving this at
// Brightness would open Settings twelve items in, below everything that moved
// here -- reachable only by scrolling backwards past the end.
int8_t settingsIdx = MENU_MODE;
#else
int8_t settingsIdx = MENU_BRIGHTNESS;
#endif

static const char *settings[] =
{
#ifdef AIRTIME
  "Mode",
  "Band",
  "Step",
  "Seek",
  "Scan",
  "Memory",
  "Squelch",
  "Bandwidth",
  "AGC/ATTN",
  "AVC",
  "SoftMute",
#endif
  "Brightness",
  "Calibration",
  "RDS",
  "UTC Offset",
  "FM Region",
  "Theme",
  "UI Layout",
  "Zoom Menu",
  "Scroll Dir.",
  "Sleep",
  "Sleep Mode",
  "Load EiBi",
  "USB Port",
  "Bluetooth",
  "Wi-Fi",
#ifdef AIRTIME
  // AirTime's own settings. These were compile-time constants until now, which
  // meant a change of location or a change of mind needed a laptop, a
  // toolchain and a reflash — the opposite of a field instrument.
  "Time Zone",
  "WWV Band",
  "HF Listen",
#endif
  "About",
};

static_assert(ITEM_COUNT(settings) == MENU_ABOUT + 1,
              "settings[] and the MENU_* indices disagree");

// The relocated block has to sit at the FRONT of settings[], because the config
// indices are all AT_SETTINGS_BASE + n and that base is what skips over it.
#ifdef AIRTIME
static_assert(MENU_SOFTMUTE + 1 == AT_SETTINGS_BASE,
              "the relocated radio items must immediately precede the settings block");
#endif

// Which array a panel title comes from.
//
// The twelve relocated items are named by the panels that edit them --
// drawVolume() draws the word "Volume" by looking it up rather than repeating
// the literal. Those lookups all said menu[...] because that is where the items
// used to live. Moving the items without moving the lookups did not fail
// loudly: menu[] is three entries long in this build, so menu[MENU_SOFTMUTE]
// reads eleven slots past the end of the array and hands whatever it finds to
// drawString() as a char*. Nine of the twelve were out of bounds; the other
// three quietly drew "Nets" and "Settings" over the wrong panels.
#ifdef AIRTIME
#define RADIO_ITEM(i) settings[i]
#else
#define RADIO_ITEM(i) menu[i]
#endif

//
// FM Region Menu
//
const FMRegion fmRegions[] = {
  // 50uS de-emphasis
  { 0x1, "EU/JP/AU" },
  // 75uS de-emphasis
  { 0x2, "US" },
};

int getTotalFmRegions() { return(ITEM_COUNT(fmRegions)); }

//
// Mode Menu
//

const char *bandModeDesc[] = { "FM", "LSB", "USB", "AM" };

int getTotalModes() { return(ITEM_COUNT(bandModeDesc)); }

//
// Memory Menu
//

uint8_t memoryIdx = 0;
Memory memories[MEMORY_COUNT];
Memory newMemory;

int getTotalMemories() { return(ITEM_COUNT(memories)); }

//
// RDS Menu
//

uint8_t rdsModeIdx = 0;
static const RDSMode rdsMode[] =
{
  { RDS_PS, "PS"},
  { RDS_PS | RDS_CT, "PS+CT" },
  { RDS_PS | RDS_PI, "PS+PI" },
  { RDS_PS | RDS_PI | RDS_CT, "PS+PI+CT" },
  { RDS_PS | RDS_PI | RDS_RT | RDS_PT, "ALL-CT (EU)" },
  { RDS_PS | RDS_PI | RDS_RT | RDS_PT | RDS_RBDS, "ALL-CT (US)" },
  { RDS_PS | RDS_PI | RDS_RT | RDS_PT | RDS_CT, "ALL (EU)" },
  { RDS_PS | RDS_PI | RDS_RT | RDS_PT | RDS_CT | RDS_RBDS, "ALL (US)" },
};

uint8_t getRDSMode() { return(rdsMode[rdsModeIdx].mode); }

//
// Sleep Mode Menu
//

uint8_t sleepModeIdx = SLEEP_LOCKED;
static const char *sleepModeDesc[] =
{ "Locked", "Unlocked", "CPU Sleep" };

//
// UTC Offset Menu
// https://en.wikipedia.org/wiki/List_of_UTC_offsets
// https://www.timeanddate.com/time/time-zones-interesting.html
//
uint8_t utcOffsetIdx = 8;
const UTCOffset utcOffsets[] =
{
  { -12 * 4, "UTC-12" },
  { -11 * 4, "UTC-11" },
  { -10 * 4, "UTC-10" },
  { -9 * 4 - 2, "UTC-9:30" },
  { -9 * 4, "UTC-9" },
  { -8 * 4, "UTC-8" },
  { -7 * 4, "UTC-7" },
  { -6 * 4, "UTC-6" },
  { -5 * 4, "UTC-5" },
  { -4 * 4, "UTC-4" },
  { -3 * 4 - 2, "UTC-3:30" },
  { -3 * 4, "UTC-3" },
  { -2 * 4 - 2, "UTC-2:30" },
  { -2 * 4, "UTC-2" },
  { -1 * 4, "UTC-1" },
  {  0 * 4, "UTC+0" },
  {  1 * 4, "UTC+1" },
  {  2 * 4, "UTC+2" },
  {  3 * 4, "UTC+3" },
  {  3 * 4 + 2, "UTC+3:30" },
  {  4 * 4, "UTC+4" },
  {  4 * 4 + 2, "UTC+4:30" },
  {  5 * 4, "UTC+5" },
  {  5 * 4 + 2, "UTC+5:30" },
  {  5 * 4 + 3, "UTC+5:45" },
  {  6 * 4, "UTC+6" },
  {  6 * 4 + 2, "UTC+6:30" },
  {  7 * 4, "UTC+7" },
  {  8 * 4, "UTC+8" },
  {  8 * 4 + 3, "UTC+8:45" },
  {  9 * 4, "UTC+9" },
  {  9 * 4 + 2, "UTC+9:30" },
  { 10 * 4, "UTC+10" },
  { 10 * 4 + 2, "UTC+10:30" },
  { 11 * 4, "UTC+11" },
  { 12 * 4, "UTC+12" },
  { 12 * 4 + 3, "UTC+12:45" },
  { 13 * 4, "UTC+13" },
  { 13 * 4 + 3, "UTC+13:45" },
  { 14 * 4, "UTC+14" },
};

int getCurrentUTCOffset() { return(utcOffsets[utcOffsetIdx].offset); }
int getTotalUTCOffsets() { return(ITEM_COUNT(utcOffsets)); }

//
// UI Layout Menu
//
uint8_t uiLayoutIdx = 0;
static const char *uiLayoutDesc[] =
{ "Default", "S-Meter" };

//
// USB Port Mode Menu
//

uint8_t usbModeIdx = USB_OFF;
static const char *usbModeDesc[] =
{ "Off", "Ad hoc" };

int getTotalUSBModes() { return(ITEM_COUNT(usbModeDesc)); }

//
// Bluetooth Mode Menu
//

uint8_t bleModeIdx = BLE_OFF;
static const char *bleModeDesc[] =
{ "Off", "Ad hoc", "HID" };

int getTotalBleModes() { return(ITEM_COUNT(bleModeDesc)); }

//
// WiFi Mode Menu
//

uint8_t wifiModeIdx = NET_OFF;
static const char *wifiModeDesc[] =
{ "Off", "AP Only", "AP+Connect", "Connect", "Sync Only" };

//
// Step Menu
//

// FM (kHz * 10)
static const Step fmSteps[] =
{
  {   1, "10k",   1 },
  {   5, "50k",   5 },
  {  10, "100k", 10 },
  {  20, "200k", 20 },
  { 100, "1M",   10 },
};

// SSB (Hz)
static const Step ssbSteps[] =
{
  {    10, "10",  1  },
  {    25, "25",  1  },
  {    50, "50",  1  },
  {   100, "100", 1  },
  {   500, "500", 1  },
  {  1000, "1k",  1  },
  {  5000, "5k",  5  },
  {  9000, "9k",  9  },
  { 10000, "10k", 10 },
};

// AM (kHz)
static const Step amSteps[] =
{
  {    1, "1k",    1 },
  {    5, "5k",    5 },
  {    9, "9k",    9 },
  {   10, "10k",  10 },
  {   50, "50k",  10 },
  {  100, "100k", 10 },
  { 1000, "1M",   10 },
};

static const Step *steps[4] = { fmSteps, ssbSteps, ssbSteps, amSteps };
static const uint8_t defaultStepIdx[4] = { 2, 5, 5, 1 };

static int getLastStep(int mode)
{
  switch(mode)
  {
    case FM:  return(LAST_ITEM(fmSteps));
    case LSB: return(LAST_ITEM(ssbSteps));
    case USB: return(LAST_ITEM(ssbSteps));
    case AM:  return(LAST_ITEM(amSteps));
  }

  return(0);
}

const Step *getCurrentStep()
{
  uint8_t idx = bands[bandIdx].currentStepIdx > getLastStep(currentMode) ? defaultStepIdx[currentMode] : bands[bandIdx].currentStepIdx;
  return(&steps[currentMode][idx]);
}

static uint8_t freqInputPos = 0;

static uint8_t getDefaultFreqInputPos(int mode, int step)
{
  return (uint8_t)(log10(step) * 2) + (mode == AM ? 6 : 0);
}

void resetFreqInputPos()
{
  freqInputPos = getDefaultFreqInputPos(currentMode, getCurrentStep()->step);
}

uint8_t getFreqInputPos()
{
  return freqInputPos;
}

int getFreqInputStep()
{
  return freqInputPos % 2 ?
    5 * pow(10, (freqInputPos - (currentMode == AM ? 6 : 0) - 1) / 2) :
            pow(10, (freqInputPos - (currentMode == AM ? 6 : 0)) / 2);
}

static uint8_t getMinFreqInputPos()
{
  return getDefaultFreqInputPos(currentMode, steps[currentMode][0].step);
}

static uint8_t getMaxFreqInputPos()
{
  return (uint8_t)log10(getCurrentBand()->maximumFreq) * 2 + (currentMode != FM ? 6 : -2);
}

//
// Bandwidth Menu
//

static const Bandwidth fmBandwidths[] =
{
  { 0, "Auto" }, // Automatic - default
  { 1, "110k" }, // Force wide (110 kHz) channel filter.
  { 2, "84k"  },
  { 3, "60k"  },
  { 4, "40k"  }
};

static const Bandwidth ssbBandwidths[] =
{
  { 4, "0.5k" },
  { 5, "1.0k" },
  { 0, "1.2k" },
  { 1, "2.2k" },
  { 2, "3.0k" },
  { 3, "4.0k" }
};

static const Bandwidth amBandwidths[] =
{
  { 4, "1.0k" },
  { 5, "1.8k" },
  { 3, "2.0k" },
  { 6, "2.5k" },
  { 2, "3.0k" },
  { 1, "4.0k" },
  { 0, "6.0k" }
};

static const Bandwidth *bandwidths[4] =
{
  fmBandwidths, ssbBandwidths, ssbBandwidths, amBandwidths
};

static const uint8_t defaultBwIdx[4] = { 0, 4, 4, 4 };

static int getLastBandwidth(int mode)
{
  switch(mode)
  {
    case FM:  return(LAST_ITEM(fmBandwidths));
    case LSB: return(LAST_ITEM(ssbBandwidths));
    case USB: return(LAST_ITEM(ssbBandwidths));
    case AM:  return(LAST_ITEM(amBandwidths));
  }

  return(0);
}

const Bandwidth *getCurrentBandwidth()
{
  return(&bandwidths[currentMode][bands[bandIdx].bandwidthIdx > getLastBandwidth(currentMode) ? defaultBwIdx[currentMode] : bands[bandIdx].bandwidthIdx]);
}

static void setBandwidth()
{
  uint8_t idx = getCurrentBandwidth()->idx;

  switch(currentMode)
  {
    case FM:
      rx.setFmBandwidth(idx);
      break;
    case AM:
      rx.setBandwidth(idx, 1);
      break;
    case LSB:
    case USB:
      // Set Audio
      rx.setSSBAudioBandwidth(idx);
      // If audio bandwidth selected is about 2 kHz or below, it is
      // recommended to set Sideband Cutoff Filter to 0.
      rx.setSSBSidebandCutoffFilter(idx==0 || idx==4 || idx==5? 0 : 1);
      break;
  }
}

// Seek mode. Pass true to toggle, false to return the current one
uint8_t seekMode(bool toggle)
{
  static uint8_t mode = SEEK_DEFAULT;

  mode = toggle ? (mode == SEEK_DEFAULT ? SEEK_SCHEDULE : SEEK_DEFAULT) : mode;

  // Use normal seek on FM or if there is no schedule loaded
  if(currentMode == FM || !eibiAvailable() || !clockAvailable())
    return(SEEK_DEFAULT);

  return(mode);
}

//
// Utility functions to change menu values
//

static inline int min(int x, int y) { return(x<y? x:y); }

static inline int wrap_range(int v, int enc, int vMin, int vMax)
{
  v += enc;
  v  = v>vMax? vMin + (v - vMax - 1) % (vMax - vMin + 1) : v<vMin? vMax - (vMin - v - 1) % (vMax - vMin + 1) : v;
  return(v);
}

static inline int clamp_range(int v, int enc, int vMin, int vMax)
{
  v += enc;
  v  = v>vMax? vMax : v<vMin? vMin : v;
  return(v);
}

//
// Encoder input handlers
//

void doSelectDigit(int16_t enc)
{
  freqInputPos = clamp_range(freqInputPos, -enc, getMinFreqInputPos(), getMaxFreqInputPos());
}

void doVolume(int16_t enc)
{
  volume = clamp_range(volume, enc, 0, 63);
  if(!muteOn(MUTE_MAIN)) rx.setVolume(volume);
}

static void clickVolume(bool shortPress)
{
  if(shortPress) muteOn(MUTE_MAIN, !muteOn(MUTE_MAIN)); else currentCmd = CMD_NONE;
}

static void clickSquelch(bool shortPress)
{
  if(shortPress)
  {
    if(currentSquelch[currentMode] & 0x7f)
      currentSquelch[currentMode] &= 0x80;
    else
      currentSquelch[currentMode] ^= 0x80;
  }
  else
  {
    currentCmd = CMD_NONE;
  }
}

static void clickSeek(bool shortPress)
{
  if(shortPress) seekMode(true); else currentCmd = CMD_NONE;
}

static void clickScan(bool shortPress)
{
  if(shortPress)
  {
    // Clear stale parameters
    clearStationInfo();
    rssi = snr = 0;
    drawScreen();
    drawMessage("Scanning...");
    scanRun(currentFrequency, 10);
  }
  else currentCmd = CMD_NONE;
}

static void doTheme(int16_t enc)
{
  themeIdx = wrap_range(themeIdx, enc, 0, getTotalThemes() - 1);
}

static void doUILayout(int16_t enc)
{
  uiLayoutIdx = uiLayoutIdx > LAST_ITEM(uiLayoutDesc) ? UI_DEFAULT : wrap_range(uiLayoutIdx, enc, 0, LAST_ITEM(uiLayoutDesc));
}

void doAvc(int16_t enc)
{
  // Only allow for AM and SSB modes
  if(currentMode==FM) return;

  // wrap_range expects to wrap a range of incremental numbers. avc instead is a range of all even numbers
  int8_t newAvcIdx = wrap_range((isSSB() ? SsbAvcIdx : AmAvcIdx) / 2, enc, 12 / 2, 90 / 2) * 2;
  if(isSSB())
  {
    SsbAvcIdx = newAvcIdx;
  }
  else
  {
    AmAvcIdx = newAvcIdx;
  }
  rx.setAvcAmMaxGain(newAvcIdx);
}

void doFmRegion(int16_t enc)
{
  // Only allow for FM mode
  if(currentMode!=FM) return;

  FmRegionIdx = wrap_range(FmRegionIdx, enc, 0, LAST_ITEM(fmRegions));
  rx.setFMDeEmphasis(fmRegions[FmRegionIdx].value);
}

void doCal(int16_t enc)
{
  if (currentMode == USB)
    bands[bandIdx].usbCal = clamp_range(bands[bandIdx].usbCal, 10*enc, -MAX_CAL, MAX_CAL);
  else if (currentMode == LSB)
    bands[bandIdx].lsbCal = clamp_range(bands[bandIdx].lsbCal, 10*enc, -MAX_CAL, MAX_CAL);
  // else: no calibration change for other modes

  // If in SSB mode set the SI4732/5 BFO value
  // This adjusts the BFO while in the calibration menu
  if(isSSB()) updateBFO(currentBFO, true);
}

void doBrt(int16_t enc)
{
  currentBrt = clamp_range(currentBrt, 5*enc, 10, 255);
  if(!sleepOn()) ledcWrite(PIN_LCD_BL, currentBrt);
}

static void doSleep(int16_t enc)
{
  currentSleep = clamp_range(currentSleep, 5*enc, 0, 255);
}

static void doSleepMode(int16_t enc)
{
  sleepModeIdx = wrap_range(sleepModeIdx, enc, 0, LAST_ITEM(sleepModeDesc));
}

static void doUSBMode(int16_t enc)
{
  usbModeIdx = wrap_range(usbModeIdx, enc, 0, LAST_ITEM(usbModeDesc));
}

static void doBleMode(int16_t enc)
{
  bleModeIdx = wrap_range(bleModeIdx, enc, 0, LAST_ITEM(bleModeDesc));
}

static void doWiFiMode(int16_t enc)
{
  wifiModeIdx = wrap_range(wifiModeIdx, enc, 0, LAST_ITEM(wifiModeDesc));
}

static void clickBleMode(uint8_t mode, bool shortPress)
{
  currentCmd = CMD_NONE;
  bleInit(mode);
}

static void clickWiFiMode(uint8_t mode, bool shortPress)
{
  currentCmd = CMD_NONE;
  netInit(mode);
}

static void doRDSMode(int16_t enc)
{
  rdsModeIdx = wrap_range(rdsModeIdx, enc, 0, LAST_ITEM(rdsMode));
  if(!(getRDSMode() & RDS_CT)) clockReset();
}

static void doUTCOffset(int16_t enc)
{
  utcOffsetIdx = wrap_range(utcOffsetIdx, enc, 0, LAST_ITEM(utcOffsets));
  clockRefreshTime();
}

static void doZoom(int16_t enc)
{
  zoomMenu = !zoomMenu;
}

static void doScrollDir(int16_t enc)
{
  scrollDirection = (scrollDirection == 1) ? -1 : 1;
}

uint8_t doAbout(int16_t enc)
{
  static uint8_t aboutScreen = 0;
  aboutScreen = clamp_range(aboutScreen, enc, 0, 2);
  return aboutScreen;
}

bool tuneToMemory(const Memory *memory)
{
  uint16_t freq = freqFromHz(memory->freq, memory->mode);
  int bfo = bfoFromHz(memory->freq);

  // Must have frequency
  if(!memory->freq) return(false);

  // Must have valid band index
  if(memory->band>=getTotalBands()) return(false);

  // Band must contain frequency and modulation
  if(!isMemoryInBand(&bands[memory->band], memory)) return(false);

  // Must differ from the current band, frequency and modulation
  // FIXME: bands should store frequency in Hz (otherwise sub-kHz
  // digits are lost after a power cycle)
  if(memory->band==bandIdx && freq==bands[bandIdx].currentFreq && memory->mode==bands[bandIdx].bandMode)
    return(true);

  // Save current band settings
  bands[bandIdx].currentFreq = currentFrequency + currentBFO / 1000;

  // Use default step when changing modes
  if(bands[memory->band].bandMode != memory->mode)
    bands[memory->band].currentStepIdx = defaultStepIdx[memory->mode];

  // Load frequency and modulation from memory slot
  bands[memory->band].currentFreq = freq;
  bands[memory->band].bandMode    = memory->mode;

  // Enable the new band
  selectBand(memory->band);

  // Update BFO if present in memory slot
  if(bfo) updateBFO(bfo);

  return(true);
}

static void doMemory(int16_t enc)
{
  memoryIdx = wrap_range(memoryIdx, enc, 0, LAST_ITEM(memories));
  if(!tuneToMemory(&memories[memoryIdx])) tuneToMemory(&newMemory);
}

static void clickMemory(uint8_t idx, bool shortPress)
{
  // Must have a valid index
  if(idx>LAST_ITEM(memories)) return;

  if(shortPress)
  {
    // If clicking on an empty memory slot, save to it
    if(!memories[idx].freq) memories[idx] = newMemory;
    // Otherwise, delete memory slot contents
    else memories[idx].freq = 0;
  }
  // On a click, do nothing, slot already activated in doMemory()
  else currentCmd = CMD_NONE;
}

void doStep(int16_t enc)
{
  uint8_t idx = bands[bandIdx].currentStepIdx;

  idx = wrap_range(idx, enc, 0, getLastStep(currentMode));
  bands[bandIdx].currentStepIdx = idx;

  rx.setFrequencyStep(steps[currentMode][idx].step);

  // Set seek spacing
  if(currentMode==FM)
    rx.setSeekFmSpacing(steps[currentMode][idx].spacing);
  else
    rx.setSeekAmSpacing(steps[currentMode][idx].spacing);
}

void doAgc(int16_t enc)
{
  if(currentMode==FM)
    agcIdx = FmAgcIdx = wrap_range(FmAgcIdx, enc, 0, 27);
  else if(isSSB())
    agcIdx = SsbAgcIdx = wrap_range(SsbAgcIdx, enc, 0, 1);
  else
    agcIdx = AmAgcIdx = wrap_range(AmAgcIdx, enc, 0, 37);

  // Process agcIdx to generate disableAgc and agcIdx
  // agcIdx     0 1 2 3 4 5 6  ..... n    (n:    FM = 27, AM = 37, SSB = 1)
  // agcNdx     0 0 1 2 3 4 5  ..... n -1 (n -1: FM = 26, AM = 36, SSB = 0)
  // disableAgc 0 1 1 1 1 1 1  ..... 1

  // if true, disable AGC; else, AGC is enabled
  disableAgc = agcIdx>0? 1 : 0;
  agcNdx     = agcIdx>1? agcIdx - 1 : 0;

  // Configure SI4732/5 (if agcNdx = 0, no attenuation)
  rx.setAutomaticGainControl(disableAgc, agcNdx);
}

void doMode(int16_t enc)
{
  // This is our current mode for the current band
  currentMode = bands[bandIdx].bandMode;

  // Cannot change away from FM mode
  if(currentMode==FM) return;

  // Change AM/LSB/USB modes, do not allow FM mode
  do
    currentMode = wrap_range(currentMode, enc, 0, LAST_ITEM(bandModeDesc));
  while(currentMode==FM);

  // Save current band settings
  bands[bandIdx].currentFreq = currentFrequency + currentBFO / 1000;
  bands[bandIdx].currentStepIdx = defaultStepIdx[currentMode];
  bands[bandIdx].bandwidthIdx = defaultBwIdx[currentMode];
  bands[bandIdx].bandMode = currentMode;

  // Enable the new band
  selectBand(bandIdx);
}

void doSquelch(int16_t enc)
{
  uint8_t squelchParam = currentSquelch[currentMode] & 0x80;
  uint8_t squelchValue = currentSquelch[currentMode] & 0x7f;
  currentSquelch[currentMode] = squelchParam | clamp_range(squelchValue, enc, 0, 0x7f);
}

void doSoftMute(int16_t enc)
{
  // Nothing to do if FM mode
  if(currentMode==FM) return;

  if(isSSB())
    softMuteMaxAttIdx = SsbSoftMuteIdx = wrap_range(SsbSoftMuteIdx, enc, 0, 32);
  else
    softMuteMaxAttIdx = AmSoftMuteIdx = wrap_range(AmSoftMuteIdx, enc, 0, 32);

  rx.setAmSoftMuteMaxAttenuation(softMuteMaxAttIdx);
}

void doBand(int16_t enc)
{
  // Save current band settings
  bands[bandIdx].currentFreq = currentFrequency + currentBFO / 1000;
  bands[bandIdx].bandMode = currentMode;

  // Change band
  bandIdx = wrap_range(bandIdx, enc, 0, LAST_ITEM(bands));

  // Enable the new band
  selectBand(bandIdx);
}

void doBandwidth(int16_t enc)
{
  uint8_t idx = bands[bandIdx].bandwidthIdx;

  idx = wrap_range(idx, enc, 0, getLastBandwidth(currentMode));
  bands[bandIdx].bandwidthIdx = idx;
  setBandwidth();
}

//
// Handle encoder input in menu
//

static void doMenu(int16_t enc)
{
  menuIdx = wrap_range(menuIdx, enc, 0, LAST_ITEM(menu));
}

static void clickMenu(int cmd, bool shortPress)
{
  // No command yet
  currentCmd = CMD_NONE;

  switch(cmd)
  {
#ifndef AIRTIME
    case MENU_STEP:     currentCmd = CMD_STEP;      break;
    case MENU_SEEK:     currentCmd = CMD_SEEK;      break;
    case MENU_MODE:     currentCmd = CMD_MODE;      break;
    case MENU_BW:       currentCmd = CMD_BANDWIDTH; break;
    case MENU_AGC_ATT:  currentCmd = CMD_AGC;       break;
    case MENU_BAND:     currentCmd = CMD_BAND;      break;
#endif
#ifdef AIRTIME
    case MENU_VOLUME:   currentCmd = CMD_VOLUME;   break;
    case MENU_MODE_AT:  currentCmd = CMD_AT_MODE;  break;
    case MENU_NETS:     currentCmd = CMD_AT_NETS;  break;
#endif
    case MENU_SETTINGS: currentCmd = CMD_SETTINGS;  break;
#ifndef AIRTIME
    case MENU_SQUELCH:  currentCmd = CMD_SQUELCH;   break;
    case MENU_VOLUME:   currentCmd = CMD_VOLUME;    break;

    case MENU_MEMORY:
      currentCmd = CMD_MEMORY;
      newMemory.freq  = freqToHz(currentFrequency, currentMode) + currentBFO;
      newMemory.mode  = currentMode;
      newMemory.band  = bandIdx;
      doMemory(0);
      break;

    case MENU_SOFTMUTE:
      // No soft mute in FM mode
      if(currentMode!=FM) currentCmd = CMD_SOFTMUTE;
      break;

    case MENU_AVC:
      // No AVC in FM mode
      if(currentMode!=FM) currentCmd = CMD_AVC;
      break;

    case MENU_SCAN:
      // Run a band scan around current frequency with the same
      // step as scale resolution (10kHz for AM, 100kHz for FM)
      currentCmd = CMD_SCAN;
      clickScan(true);
      break;
#endif
  }
}

static void doSettings(int16_t enc)
{
  settingsIdx = wrap_range(settingsIdx, enc, 0, LAST_ITEM(settings));
}

#ifdef AIRTIME
// AirTime menu items. Each is a plain list: the AirTime side owns the data and
// the meaning, this side owns only the turning and the drawing — which is why
// one renderer serves all three.
static void doAtZone(int16_t enc) { atSetZoneIdx(wrap_range(atZoneIdx(), enc, 0, atZoneCount() - 1)); }
static void doAtBand(int16_t enc) { atSetBandIdx(wrap_range(atBandIdx(), enc, 0, atBandCount() - 1)); }
static void doAtHf(int16_t enc)   { atSetHfIdx(wrap_range(atHfIdx(), enc, 0, atHfCount() - 1)); }
static void doAtMode(int16_t enc) { atSetModeIdx(wrap_range(atModeIdx(), enc, 0, atModeCount() - 1)); }
static void doAtNets(int16_t enc) { atSetNetIdx(wrap_range(atNetIdx(), enc, 0, atNetCount() - 1)); }

// Nets is the one AirTime list where scrolling is not the whole interaction.
// The others are settings -- turning the encoder IS the change -- but a net is
// a place to go, so it needs a click to mean something. Without this the list
// scrolled, showed "14300 kHz ON AIR NOW", and did precisely nothing when
// chosen: clickHandler() fell through to its default, which closes the menu.
static void clickAtNets(int idx, bool shortPress)
{
  (void)shortPress;        // either press means "take me there"
  atTuneNet(idx);
  currentCmd = CMD_NONE;   // out of the way; the operator wants the radio now
}

#endif

static void clickSettings(int cmd, bool shortPress)
{
  // No command yet
  currentCmd = CMD_NONE;

  switch(cmd)
  {
#ifdef AIRTIME
    // Relocated from the root menu. These are the stock bodies verbatim, not
    // paraphrases -- Memory has to snapshot the dial before the list opens or
    // it stores whatever was in newMemory last time, and SoftMute/AVC are
    // silently absent in FM because the chip has no such control there.
    case MENU_STEP:     currentCmd = CMD_STEP;      break;
    case MENU_SEEK:     currentCmd = CMD_SEEK;      break;
    case MENU_MODE:     currentCmd = CMD_MODE;      break;
    case MENU_BW:       currentCmd = CMD_BANDWIDTH; break;
    case MENU_AGC_ATT:  currentCmd = CMD_AGC;       break;
    case MENU_BAND:     currentCmd = CMD_BAND;      break;
    case MENU_SQUELCH:  currentCmd = CMD_SQUELCH;   break;

    case MENU_MEMORY:
      currentCmd = CMD_MEMORY;
      newMemory.freq  = freqToHz(currentFrequency, currentMode) + currentBFO;
      newMemory.mode  = currentMode;
      newMemory.band  = bandIdx;
      doMemory(0);
      break;

    case MENU_SOFTMUTE:
      // No soft mute in FM mode
      if(currentMode!=FM) currentCmd = CMD_SOFTMUTE;
      break;

    case MENU_AVC:
      // No AVC in FM mode
      if(currentMode!=FM) currentCmd = CMD_AVC;
      break;

    case MENU_SCAN:
      // Run a band scan around current frequency with the same
      // step as scale resolution (10kHz for AM, 100kHz for FM)
      currentCmd = CMD_SCAN;
      clickScan(true);
      break;
#endif
    case MENU_BRIGHTNESS: currentCmd = CMD_BRT; break;
    case MENU_CALIBRATION:
      if(isSSB()) currentCmd = CMD_CAL;
      break;
    case MENU_THEME:      currentCmd = CMD_THEME;      break;
    case MENU_UI:         currentCmd = CMD_UI;         break;
    case MENU_RDS:        currentCmd = CMD_RDS;        break;
    case MENU_ZOOM:       currentCmd = CMD_ZOOM;       break;
    case MENU_SCROLL:     currentCmd = CMD_SCROLL;     break;
    case MENU_SLEEP:      currentCmd = CMD_SLEEP;      break;
    case MENU_SLEEPMODE:  currentCmd = CMD_SLEEPMODE;  break;
    case MENU_UTCOFFSET:  currentCmd = CMD_UTCOFFSET;  break;
    case MENU_USBMODE:    currentCmd = CMD_USBMODE;    break;
    case MENU_BLEMODE:    currentCmd = CMD_BLEMODE;    break;
    case MENU_WIFIMODE:   currentCmd = CMD_WIFIMODE;   break;
#ifdef AIRTIME
    case MENU_AT_ZONE:    currentCmd = CMD_AT_ZONE;    break;
    case MENU_AT_BAND:    currentCmd = CMD_AT_BAND;    break;
    case MENU_AT_HF:      currentCmd = CMD_AT_HF;      break;
#endif
    case MENU_FM_REGION:
      // Only in FM mode
      if(currentMode==FM) currentCmd = CMD_FM_REGION;
      break;
    case MENU_ABOUT:      currentCmd = CMD_ABOUT;     break;

    case MENU_LOADEIBI:
      eibiLoadSchedule();
      break;
  }
}

bool doSideBar(uint16_t cmd, int16_t enc, int16_t enca)
{
  // Ignore idle encoder
  if(!enc) return(false);

  switch(cmd)
  {
    // Menus and list-based options must take scrollDirection into account
    case CMD_MENU:       doMenu(scrollDirection * enc);break;
    case CMD_MODE:       doMode(scrollDirection * enc);break;
    case CMD_STEP:       doStep(scrollDirection * enc);break;
    case CMD_AGC:        doAgc(enc);break;
    case CMD_BANDWIDTH:  doBandwidth(scrollDirection * enc);break;
    case CMD_VOLUME:     doVolume(enca);break;
    case CMD_SOFTMUTE:   doSoftMute(enc);break;
    case CMD_BAND:       doBand(scrollDirection * enc);break;
    case CMD_AVC:        doAvc(enc);break;
    case CMD_FM_REGION:  doFmRegion(scrollDirection * enc);break;
    case CMD_SETTINGS:   doSettings(scrollDirection * enc);break;
    case CMD_BRT:        doBrt(enca);break;
    case CMD_CAL:        doCal(enca);break;
    case CMD_THEME:      doTheme(scrollDirection * enc);break;
    case CMD_UI:         doUILayout(scrollDirection * enc);break;
    case CMD_RDS:        doRDSMode(scrollDirection * enc);break;
    case CMD_MEMORY:     doMemory(scrollDirection * enca);break;
    case CMD_SLEEP:      doSleep(enca);break;
    case CMD_SLEEPMODE:  doSleepMode(scrollDirection * enc);break;
    case CMD_USBMODE:    doUSBMode(scrollDirection * enc);break;
    case CMD_BLEMODE:    doBleMode(scrollDirection * enc);break;
    case CMD_WIFIMODE:   doWiFiMode(scrollDirection * enc);break;
    case CMD_ZOOM:       doZoom(enc);break;
    case CMD_SCROLL:     doScrollDir(enc);break;
    case CMD_UTCOFFSET:  doUTCOffset(scrollDirection * enc);break;
#ifdef AIRTIME
    case CMD_AT_ZONE:    doAtZone(scrollDirection * enc);break;
    case CMD_AT_BAND:    doAtBand(scrollDirection * enc);break;
    case CMD_AT_HF:      doAtHf(scrollDirection * enc);break;
    case CMD_AT_MODE:    doAtMode(scrollDirection * enc);break;
    case CMD_AT_NETS:    doAtNets(scrollDirection * enc);break;
#endif
    case CMD_SQUELCH:    doSquelch(enca);break;
    case CMD_ABOUT:      doAbout(enc);break;
    default:             return(false);
  }

  // Encoder input handled
  return(true);
}

bool clickHandler(uint16_t cmd, bool shortPress)
{
  switch(cmd)
  {
    case CMD_MENU:     clickMenu(menuIdx, shortPress);break;
    case CMD_SETTINGS: clickSettings(settingsIdx, shortPress);break;
#ifdef AIRTIME
    case CMD_AT_NETS:  clickAtNets(atNetIdx(), shortPress);break;
#endif
    case CMD_MEMORY:   clickMemory(memoryIdx, shortPress);break;
    case CMD_BLEMODE:  clickBleMode(bleModeIdx, shortPress);break;
    case CMD_WIFIMODE: clickWiFiMode(wifiModeIdx, shortPress);break;
    case CMD_VOLUME:   clickVolume(shortPress);break;
    case CMD_SQUELCH:  clickSquelch(shortPress);break;
    case CMD_SEEK:     clickSeek(shortPress);break;
    case CMD_SCAN:     clickScan(shortPress);break;
    case CMD_FREQ:     return(clickFreq(shortPress));
    default:           return(false);
  }

  // Encoder input handled
  return(true);
}

//
// Selecting given band
//

void selectBand(uint8_t idx, bool drawLoadingSSB)
{
  // Silence click on some hardware versions
  // https://github.com/esp32-si4732/ats-mini/discussions/103
  muteOn(MUTE_TEMP, true);

  // Set band and mode
  bandIdx = min(idx, LAST_ITEM(bands));
  currentMode = bands[bandIdx].bandMode;

  // Load SSB patch as needed
  if(isSSB())
    loadSSB(getCurrentBandwidth()->idx, drawLoadingSSB);
  else
    unloadSSB();

  // Switch radio to the selected band
  useBand(&bands[bandIdx]);

  // Set bandwidth for the current mode
  setBandwidth();

  // Clear current station info (RDS/CB)
  clearStationInfo();

  // Check for named frequencies
  identifyFrequency(currentFrequency + currentBFO / 1000);

  // Set default digit position based on the current step
  resetFreqInputPos();

  // Unmute the sound
  muteOn(MUTE_TEMP, false);
}

//
// Draw functions
//

static void drawCommon(const char *title, int x, int y, int sx, bool cursor = false)
{
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_hdr);
  spr.fillSmoothRoundRect(1+x, 1+y, 76+sx, 110, 4, TH.menu_border);
  spr.fillSmoothRoundRect(2+x, 2+y, 74+sx, 108, 4, TH.menu_bg);

  spr.drawString(title, 40+x+(sx/2), 12+y, 2);
  spr.drawLine(1+x, 23+y, 76+sx, 23+y, TH.menu_border);

  spr.setTextFont(0);
  spr.setTextColor(TH.menu_item);
  if(cursor)
    spr.fillRoundRect(6+x, 24+y+(2*16), 66+sx, 16, 2, TH.menu_hl_bg);
}

static void drawMenu(int x, int y, int sx)
{
  spr.setTextDatum(MC_DATUM);

  spr.fillSmoothRoundRect(1+x, 1+y, 76+sx, 110, 4, TH.menu_border);
  spr.fillSmoothRoundRect(2+x, 2+y, 74+sx, 108, 4, TH.menu_bg);
  spr.setTextColor(TH.menu_hdr);

  spr.drawString("Menu", 40+x+(sx/2), 12+y, 2);
  spr.drawLine(1+x, 23+y, 76+sx, 23+y, TH.menu_border);

  spr.setTextFont(0);
  spr.setTextColor(TH.menu_item);
  spr.fillRoundRect(6+x, 24+y+(2*16), 66+sx, 16, 2, TH.menu_hl_bg);

  int count = ITEM_COUNT(menu);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(menu[abs((menuIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }
    spr.setTextDatum(MC_DATUM);
    spr.drawString(menu[abs((menuIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawSettings(int x, int y, int sx)
{
  spr.setTextDatum(MC_DATUM);

  spr.fillSmoothRoundRect(1+x, 1+y, 76+sx, 110, 4, TH.menu_border);
  spr.fillSmoothRoundRect(2+x, 2+y, 74+sx, 108, 4, TH.menu_bg);
  spr.setTextColor(TH.menu_hdr);
  spr.drawString("Settings", 40+x+(sx/2), 12+y, 2);
  spr.drawLine(1+x, 23+y, 76+sx, 23+y, TH.menu_border);

  spr.setTextFont(0);
  spr.setTextColor(TH.menu_item);
  spr.fillRoundRect(6+x, 24+y+(2*16), 66+sx, 16, 2, TH.menu_hl_bg);

  int count = ITEM_COUNT(settings);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(settings[abs((settingsIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(settings[abs((settingsIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawMode(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_MODE), x, y, sx, true);

  int count = ITEM_COUNT(bandModeDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(bandModeDesc[abs((currentMode+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    if((currentMode!=FM) || (i==0))
     spr.drawString(bandModeDesc[abs((currentMode+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawStep(int x, int y, int sx)
{
  int count = getLastStep(currentMode) + 1;
  int idx   = bands[bandIdx].currentStepIdx + count;

  drawCommon(RADIO_ITEM(MENU_STEP), x, y, sx, true);

  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(steps[currentMode][abs((idx+i)%count)].desc);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(steps[currentMode][abs((idx+i)%count)].desc, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawSeek(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_SEEK), x, y, sx);
  spr.drawSmoothArc(40+x+(sx/2), 66+y, 30, 27, 45, 180, TH.menu_param, TH.menu_bg);
  spr.fillTriangle(40+x+(sx/2)-5, 66+y-32, 40+x+(sx/2)+5, 66+y-27, 40+x+(sx/2)-5, 66+y-22, TH.menu_param);
  spr.drawSmoothArc(40+x+(sx/2), 66+y, 30, 27, 225, 360, TH.menu_param, TH.menu_bg);
  spr.fillTriangle(40+x+(sx/2)+5, 66+y+32, 40+x+(sx/2)-5, 66+y+27, 40+x+(sx/2)+5, 66+y+22, TH.menu_param);

  if(seekMode()==SEEK_SCHEDULE)
  {
    spr.drawCircle(40+x+(sx/2), 66+y, 10, TH.menu_param);
    spr.drawLine(40+x+(sx/2), 66+y, 40+x+(sx/2), 66+y-7, TH.menu_param);
    spr.drawLine(40+x+(sx/2), 66+y, 40+x+(sx/2)+4, 66+y+4, TH.menu_param);
  }
}

static void drawScan(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_SCAN), x, y, sx);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TH.scan_rssi);
  spr.drawString("S", 40+x+(sx/2)-30, 66+y+30, 2);
  spr.setTextColor(TH.scan_snr);
  spr.drawString("N", 40+x+(sx/2)+30, 66+y+30, 2);

  spr.drawSmoothArc(40+x+(sx/2), 66+y, 30, 27, 45, 180, TH.menu_param, TH.menu_bg);
  spr.fillTriangle(40+x+(sx/2)-5, 66+y-32, 40+x+(sx/2)+5, 66+y-27, 40+x+(sx/2)-5, 66+y-22, TH.menu_param);
  spr.drawSmoothArc(40+x+(sx/2), 66+y, 30, 27, 225, 360, TH.menu_param, TH.menu_bg);
  spr.fillTriangle(40+x+(sx/2)+5, 66+y+32, 40+x+(sx/2)-5, 66+y+27, 40+x+(sx/2)+5, 66+y+22, TH.menu_param);

  spr.drawLine(40+x+(sx/2)-17, 66+y+5, 40+x+(sx/2)-4, 66+y+5, TH.menu_param);
  spr.drawLine(40+x+(sx/2)-4, 66+y+5, 40+x+(sx/2), 66+y-16+5, TH.menu_param);
  spr.drawLine(40+x+(sx/2), 66+y-16+5, 40+x+(sx/2)+4, 66+y+5, TH.menu_param);
  spr.drawLine(40+x+(sx/2)+4, 66+y+5, 40+x+(sx/2)+17, 66+y+5, TH.menu_param);
}

static void drawBand(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_BAND), x, y, sx, true);

  int count = ITEM_COUNT(bands);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(bands[abs((bandIdx+count+i)%count)].bandName);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(bands[abs((bandIdx+count+i)%count)].bandName, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawBandwidth(int x, int y, int sx)
{
  int count = getLastBandwidth(currentMode) + 1;
  int idx   = bands[bandIdx].bandwidthIdx + count;

  drawCommon(RADIO_ITEM(MENU_BW), x, y, sx, true);

  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(bandwidths[currentMode][abs((idx+i)%count)].desc);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(bandwidths[currentMode][abs((idx+i)%count)].desc, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawSleepMode(int x, int y, int sx)
{
  drawCommon(settings[MENU_SLEEPMODE], x, y, sx, true);

  int count = ITEM_COUNT(sleepModeDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(sleepModeDesc[abs((sleepModeIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(sleepModeDesc[abs((sleepModeIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawUSBMode(int x, int y, int sx)
{
  drawCommon(settings[MENU_USBMODE], x, y, sx, true);

  int count = ITEM_COUNT(usbModeDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(usbModeDesc[abs((usbModeIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    // Prevent repeats for short menus
    if (count < 5 && ((usbModeIdx+i) < 0 || (usbModeIdx+i) >= count)) {
      continue;
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(usbModeDesc[abs((usbModeIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawBleMode(int x, int y, int sx)
{
  drawCommon(settings[MENU_BLEMODE], x, y, sx, true);

  int count = ITEM_COUNT(bleModeDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(bleModeDesc[abs((bleModeIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    // Prevent repeats for short menus
    if (count < 5 && ((bleModeIdx+i) < 0 || (bleModeIdx+i) >= count)) {
      continue;
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(bleModeDesc[abs((bleModeIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawWiFiMode(int x, int y, int sx)
{
  drawCommon(settings[MENU_WIFIMODE], x, y, sx, true);

  int count = ITEM_COUNT(wifiModeDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(wifiModeDesc[abs((wifiModeIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(wifiModeDesc[abs((wifiModeIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawTheme(int x, int y, int sx)
{
  drawCommon(settings[MENU_THEME], x, y, sx, true);

  int count = getTotalThemes();
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(theme[abs((themeIdx+count+i)%count)].name);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(theme[abs((themeIdx+count+i)%count)].name, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawUILayout(int x, int y, int sx)
{
  drawCommon(settings[MENU_UI], x, y, sx, true);

  int count = ITEM_COUNT(uiLayoutDesc);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(uiLayoutDesc[abs((uiLayoutIdx+count+i)%count)]);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    // Prevent repeats for short menus
    if (count < 5 && ((uiLayoutIdx+i) < 0 || (uiLayoutIdx+i) >= count)) {
      continue;
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(uiLayoutDesc[abs((uiLayoutIdx+count+i)%count)], 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawRDSMode(int x, int y, int sx)
{
  drawCommon(settings[MENU_RDS], x, y, sx, true);

  int count = ITEM_COUNT(rdsMode);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(rdsMode[abs((rdsModeIdx+count+i)%count)].desc);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(rdsMode[abs((rdsModeIdx+count+i)%count)].desc, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawUTCOffset(int x, int y, int sx)
{
  drawCommon(settings[MENU_UTCOFFSET], x, y, sx, true);

  int count = ITEM_COUNT(utcOffsets);
  uint8_t idx = utcOffsetIdx;

  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0)
    {
      drawZoomedMenu(utcOffsets[abs((idx+count+i)%count)].desc);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    }
    else
    {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(utcOffsets[abs((idx+count+i)%count)].desc, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawMemory(int x, int y, int sx)
{
  char label_memory[16];
  sprintf(label_memory, "%s %2.2d", RADIO_ITEM(MENU_MEMORY), memoryIdx + 1);
  drawCommon(label_memory, x, y, sx, true);

  int count = ITEM_COUNT(memories);
  for(int i=-2 ; i<3 ; i++)
  {
    int j = abs((memoryIdx+count+i)%count);
    char buf[16];
    const char *text = buf;

    if(!memories[j].freq)
      text = "- - -";
    else if(memories[j].mode==FM)
      sprintf(buf, "%3.2f %s", memories[j].freq / 1000000.0, bandModeDesc[memories[j].mode]);
    else
      sprintf(buf, "%5lu %s", memories[j].freq / 1000, bandModeDesc[memories[j].mode]);

    if(i==0) {
      drawZoomedMenu(text);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(text, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawVolume(int x, int y, int sx)
{
  drawCommon(menu[MENU_VOLUME], x, y, sx);
  drawZoomedMenu(menu[MENU_VOLUME]);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawNumber(volume, 40+x+(sx/2), 66+y, 7);

  if(muteOn(MUTE_MAIN))
  {
    for(int i=-3; i<4; i++)
    {
      spr.drawLine(40+x+(sx/2) + 30 + i, 66 + y - 30 + i, 40+x+(sx/2) - 30 + i, 66 + y + 30 + i, TH.menu_param);
    }
  }
}

static void drawAgc(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_AGC_ATT), x, y, sx);
  drawZoomedMenu(RADIO_ITEM(MENU_AGC_ATT));
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TH.menu_param);

  // G8PTN: Read back value is not used
  // rx.getAutomaticGainControl();
  if(!agcNdx && !agcIdx)
  {
    spr.setFreeFont(&Orbitron_Light_24);
    spr.drawString("AGC", 40+x+(sx/2), 48+y);
    spr.drawString("On", 40+x+(sx/2), 72+y);
    spr.setTextFont(0);
  }
  else
  {
    char text[16];
    sprintf(text, "%2.2d", agcNdx);
    spr.drawString(text, 40+x+(sx/2), 60+y, 7);
  }
}

static void drawSquelch(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_SQUELCH), x, y, sx);
  drawZoomedMenu(RADIO_ITEM(MENU_SQUELCH));
  spr.setTextDatum(MC_DATUM);

  uint8_t squelchValue = currentSquelch[currentMode] & 0x7f;
  bool squelchParam = currentSquelch[currentMode] & 0x80;
  if(squelchValue)
  {
    spr.drawNumber(squelchValue, 40+x+(sx/2), 60+y, 4);
    spr.drawString(squelchParam? "dB":"dBuV", 40+x+(sx/2), 90+y, 4);
  }
  else
  {
    spr.drawString("Off", 40+x+(sx/2), 60+y, 4);
    spr.drawString(squelchParam? "(snr)":"(rssi)", 40+x+(sx/2), 90+y, 4);
  }
}

static void drawSoftMuteMaxAtt(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_SOFTMUTE), x, y, sx);
  drawZoomedMenu(RADIO_ITEM(MENU_SOFTMUTE));
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawString("Max Attn", 40+x+(sx/2), 32+y, 2);
  spr.drawNumber(softMuteMaxAttIdx, 40+x+(sx/2), 60+y, 4);
  spr.drawString("dB", 40+x+(sx/2), 90+y, 4);
}

static void drawCal(int x, int y, int sx)
{
  drawCommon(settings[MENU_CALIBRATION], x, y, sx);
  drawZoomedMenu(settings[MENU_CALIBRATION]);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  if (currentMode == USB)
  {
    spr.drawString("USB", 40+x+(sx/2), 35+y, 2);
    spr.drawNumber(getCurrentBand()->usbCal, 40+x+(sx/2), 65+y, 4);
  }
  else if (currentMode == LSB)
  {
    spr.drawString("LSB", 40+x+(sx/2), 35+y, 2);
    spr.drawNumber(getCurrentBand()->lsbCal, 40+x+(sx/2), 65+y, 4);
  }
  else
    spr.drawNumber(0, 40+x+(sx/2), 65+y, 4);  // Display zero or nothing for other modes

  spr.drawString("Hz", 40+x+(sx/2), 95+y, 4);
}

static void drawAvc(int x, int y, int sx)
{
  drawCommon(RADIO_ITEM(MENU_AVC), x, y, sx);
  drawZoomedMenu(RADIO_ITEM(MENU_AVC));
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawString("Max Gain", 40+x+(sx/2), 32+y, 2);

  // Only show AVC for AM and SSB modes
  if(currentMode!=FM)
  {
    int currentAvc = isSSB()? SsbAvcIdx : AmAvcIdx;
    spr.drawNumber(currentAvc, 40+x+(sx/2), 60+y, 4);
    spr.drawString("dB", 40+x+(sx/2), 90+y, 4);
  }
}

static void drawFmRegion(int x, int y, int sx)
{
  drawCommon(settings[MENU_FM_REGION], x, y, sx, true);

  int count = ITEM_COUNT(fmRegions);
  for(int i=-2 ; i<3 ; i++)
  {
    if(i==0) {
      drawZoomedMenu(fmRegions[abs((FmRegionIdx+count+i)%count)].desc);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }

    // Prevent repeats for short menus
    if (count < 5 && ((FmRegionIdx+i) < 0 || (FmRegionIdx+i) >= count)) {
      continue;
    }

    spr.setTextDatum(MC_DATUM);
    spr.drawString(fmRegions[abs((FmRegionIdx+count+i)%count)].desc, 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawBrt(int x, int y, int sx)
{
  drawCommon(settings[MENU_BRIGHTNESS], x, y, sx);
  drawZoomedMenu(settings[MENU_BRIGHTNESS]);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawNumber(currentBrt, 40+x+(sx/2), 60+y, 4);
}

static void drawSleep(int x, int y, int sx)
{
  drawCommon(settings[MENU_SLEEP], x, y, sx);
  drawZoomedMenu(settings[MENU_SLEEP]);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawNumber(currentSleep, 40+x+(sx/2), 60+y, 4);
}

static void drawZoom(int x, int y, int sx)
{
  drawCommon(settings[MENU_ZOOM], x, y, sx);
  drawZoomedMenu(settings[MENU_ZOOM]);
  spr.setTextDatum(MC_DATUM);

  spr.setTextColor(TH.menu_param);
  spr.drawString(zoomMenu ? "On" : "Off", 40+x+(sx/2), 60+y, 4);
}

static void drawScrollDir(int x, int y, int sx)
{
  drawCommon(settings[MENU_SCROLL], x, y, sx);
  drawZoomedMenu(settings[MENU_SCROLL]);

  spr.fillRect(37+x+(sx/2), 45+y, 5, 40, TH.menu_param);
  if(scrollDirection>0)
    spr.fillTriangle(39+x+(sx/2)-5, 45+y, 39+x+(sx/2)+5, 45+y, 39+x+(sx/2), 45+y-5, TH.menu_param);
  else
    spr.fillTriangle(39+x+(sx/2)-5, 85+y, 39+x+(sx/2)+5, 85+y, 39+x+(sx/2), 85+y+5, TH.menu_param);
}

static void drawInfo(int x, int y, int sx)
{
  char text[16];

  // Info box
  spr.setTextDatum(ML_DATUM);
  spr.setTextColor(TH.box_text);
  spr.fillSmoothRoundRect(1+x, 1+y, 76+sx, 110, 4, TH.box_border);
  spr.fillSmoothRoundRect(2+x, 2+y, 74+sx, 108, 4, TH.box_bg);

  spr.drawString("Step:", 6+x, 64+y+(-3*16), 2);
  spr.drawString(getCurrentStep()->desc, 48+x, 64+y+(-3*16), 2);

  spr.drawString("BW:", 6+x, 64+y+(-2*16), 2);
  spr.drawString(getCurrentBandwidth()->desc, 48+x, 64+y+(-2*16), 2);

  if(!agcNdx && !agcIdx)
  {
    spr.drawString("AGC:", 6+x, 64+y+(-1*16), 2);
    spr.drawString("On", 48+x, 64+y+(-1*16), 2);
  }
  else
  {
    sprintf(text, "%2.2d", agcNdx);
    spr.drawString("Att:", 6+x, 64+y+(-1*16), 2);
    spr.drawString(text, 48+x, 64+y+(-1*16), 2);
  }

  spr.drawString("Vol:", 6+x, 64+y+(0*16), 2);
  if(muteOn(MUTE_MAIN) || muteOn(MUTE_SQUELCH))
  {
    spr.setTextColor(TH.box_off_text, TH.box_off_bg);
    sprintf(text, muteOn(MUTE_MAIN) ? "Muted" : "%d/sq", volume);
    spr.drawString(text, 48+x, 64+y+(0*16), 2);
    spr.setTextColor(TH.box_text);
  }
  else
  {
    spr.setTextColor(TH.box_text);
    spr.drawNumber(volume, 48+x, 64+y+(0*16), 2);
  }

  // Draw RDS PI code, if present
  uint16_t piCode = getRdsPiCode();
  if(piCode && currentMode == FM)
  {
    sprintf(text, "%04X", piCode);
    spr.drawString("PI:", 6+x, 64+y + (1*16), 2);
    spr.drawString(text, 48+x, 64+y + (1*16), 2);
  }
  else
  {
    spr.drawString("AVC:", 6+x, 64+y + (1*16), 2);

    if(currentMode==FM)
      sprintf(text, "n/a");
    else if(isSSB())
      sprintf(text, "%2.2ddB", SsbAvcIdx);
    else
      sprintf(text, "%2.2ddB", AmAvcIdx);

    spr.drawString(text, 48+x, 64+y + (1*16), 2);
  }

  // Draw current time
  if(clockGet())
  {
    spr.drawString("Time:", 6+x, 64+y+(2*16), 2);
    spr.drawString(clockGet(), 48+x, 64+y+(2*16), 2);
  }
}

//
// Draw side bar (menu or information)
//
#ifdef AIRTIME
// Root-menu lists take their title by string rather than by settings[] index.
static void drawAtRootList(const char *title, int count, int idx,
                           const char *(*name)(int), int x, int y, int sx)
{
  drawCommon(title, x, y, sx, true);
  if(count <= 0) return;
  for(int i=-2 ; i<3 ; i++)
  {
    if(count < 5 && ((idx+i) < 0 || (idx+i) >= count)) continue;
    const int j = abs((idx + count + i) % count);
    if(i==0) {
      if(strlen(name(j)) <= 12) drawZoomedMenu(name(j));
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }
    spr.setTextDatum(MC_DATUM);
    spr.drawString(name(j), 40+x+(sx/2), 64+y+(i*16), 2);
  }
}

static void drawAtNets(int x, int y, int sx)
{
  drawAtRootList(menu[MENU_NETS], atNetCount(), atNetIdx(), atNetName, x, y, sx);
}

static void drawAtList(int title, int count, int idx, const char *(*name)(int),
                       int x, int y, int sx)
{
  drawCommon(settings[title], x, y, sx, true);
  if(count <= 0) return;

  for(int i=-2 ; i<3 ; i++)
  {
    if(count < 5 && ((idx+i) < 0 || (idx+i) >= count)) continue;
    const int j = abs((idx + count + i) % count);
    if(i==0) {
      // The zoom box is a fixed 152 px at font 4. Anything longer than it can
      // hold spills off the panel, which is what the first nets build did.
      if(strlen(name(j)) <= 12) drawZoomedMenu(name(j));
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    } else {
      spr.setTextColor(TH.menu_item);
    }
    spr.setTextDatum(MC_DATUM);
    spr.drawString(name(j), 40+x+(sx/2), 64+y+(i*16), 2);
  }
}
#endif

void drawSideBar(uint16_t cmd, int x, int y, int sx)
{
  if(sleepOn()) return;

  switch(cmd)
  {
#ifdef AIRTIME
    case CMD_AT_ZONE:    drawAtList(MENU_AT_ZONE, atZoneCount(), atZoneIdx(), atZoneName, x, y, sx); break;
    case CMD_AT_BAND:    drawAtList(MENU_AT_BAND, atBandCount(), atBandIdx(), atBandName, x, y, sx); break;
    case CMD_AT_HF:      drawAtList(MENU_AT_HF,   atHfCount(),   atHfIdx(),   atHfName,   x, y, sx); break;
    case CMD_AT_MODE:    drawAtRootList(menu[MENU_MODE_AT], atModeCount(), atModeIdx(), atModeName, x, y, sx); break;
    case CMD_AT_NETS:    drawAtNets(x, y, sx); break;
#endif
    case CMD_MENU:       drawMenu(x, y, sx);       break;
    case CMD_SETTINGS:   drawSettings(x, y, sx);   break;
    case CMD_MODE:       drawMode(x, y, sx);       break;
    case CMD_STEP:       drawStep(x, y, sx);       break;
    case CMD_SEEK:       drawSeek(x, y, sx);       break;
    case CMD_SCAN:       drawScan(x, y, sx);       break;
    case CMD_BAND:       drawBand(x, y, sx);       break;
    case CMD_BANDWIDTH:  drawBandwidth(x, y, sx);  break;
    case CMD_THEME:      drawTheme(x, y, sx);      break;
    case CMD_UI:         drawUILayout(x, y, sx);   break;
    case CMD_VOLUME:     drawVolume(x, y, sx);     break;
    case CMD_AGC:        drawAgc(x, y, sx);        break;
    case CMD_SOFTMUTE:   drawSoftMuteMaxAtt(x, y, sx);break;
    case CMD_CAL:        drawCal(x, y, sx);        break;
    case CMD_AVC:        drawAvc(x, y, sx);        break;
    case CMD_FM_REGION:  drawFmRegion(x, y, sx);   break;
    case CMD_BRT:        drawBrt(x, y, sx);        break;
    case CMD_RDS:        drawRDSMode(x, y, sx);    break;
    case CMD_MEMORY:     drawMemory(x, y, sx);     break;
    case CMD_SLEEP:      drawSleep(x, y, sx);      break;
    case CMD_SLEEPMODE:  drawSleepMode(x, y, sx);  break;
    case CMD_USBMODE:    drawUSBMode(x, y, sx);    break;
    case CMD_BLEMODE:    drawBleMode(x, y, sx);    break;
    case CMD_WIFIMODE:   drawWiFiMode(x, y, sx);   break;
    case CMD_ZOOM:       drawZoom(x, y, sx);       break;
    case CMD_SCROLL:     drawScrollDir(x, y, sx);  break;
    case CMD_UTCOFFSET:  drawUTCOffset(x, y, sx);  break;
    case CMD_SQUELCH:    drawSquelch(x, y, sx);    break;
    default:             drawInfo(x, y, sx);       break;
  }
}
