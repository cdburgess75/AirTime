#ifndef MENU_H
#define MENU_H

#include "Common.h"

// Number of memory slots
#define MEMORY_COUNT  99

// Band Types
#define FM_BAND_TYPE  0
#define MW_BAND_TYPE  1
#define SW_BAND_TYPE  2
#define LW_BAND_TYPE  3

// Commands
#define CMD_NONE       0x0000
#define CMD_FREQ       0x0100
#define CMD_BAND       0x1000 //-MENU MODE starts here
#define CMD_VOLUME     0x1100 // |
#define CMD_AGC        0x1200 // |
#define CMD_BANDWIDTH  0x1300 // |
#define CMD_STEP       0x1400 // |
#define CMD_MODE       0x1500 // |
#define CMD_MENU       0x1600 // |
#define CMD_SOFTMUTE   0x1700 // |
#define CMD_AVC        0x1800 // |
#define CMD_MEMORY     0x1900 // |
#define CMD_SEEK       0x1A00 // |
#define CMD_SCAN       0x1B00 // |
#define CMD_SQUELCH    0x1C00 //-+
#define CMD_SETTINGS   0x2000 //-SETTINGS MODE starts here
#define CMD_BRT        0x2100 // |
#define CMD_CAL        0x2200 // |
#define CMD_RDS        0x2300 // |
#define CMD_UTCOFFSET  0x2400 // |
#define CMD_FM_REGION  0x2500 // |
#define CMD_THEME      0x2600 // |
#define CMD_UI         0x2700 // |
#define CMD_ZOOM       0x2800 // |
#define CMD_SCROLL     0x2900 // |
#define CMD_SLEEP      0x2A00 // |
#define CMD_SLEEPMODE  0x2B00 // |
#define CMD_LOADEIBI   0x2C00 // |
#define CMD_USBMODE    0x2D00 // |
#define CMD_BLEMODE    0x2E00 // |
#define CMD_WIFIMODE   0x2F00 // |
#ifdef AIRTIME
#define CMD_AT_ZONE    0x3000 // | AirTime: operator time zone
#define CMD_AT_BAND    0x3100 // | AirTime: WWV band to try first
#define CMD_AT_HF      0x3200 // | AirTime: listen now / serve now
#define CMD_AT_MODE    0x3250 // | AirTime: clock or receiver
#define CMD_AT_NETS    0x3280 // | AirTime: scheduled nets
#define CMD_AT_RESET   0x32C0 // | AirTime: wipe learned state and reboot
#endif
#define CMD_ABOUT      0x3300 //-+

// UI Layouts
#define UI_DEFAULT  0
#define UI_SMETER   1

// Seek modes
#define SEEK_DEFAULT  0
#define SEEK_SCHEDULE 1

//
// Data Types
//

typedef struct
{
  uint8_t idx;      // SI473X device bandwidth index
  const char *desc; // Bandwidth description
} Bandwidth;

typedef struct
{
  int step;         // Step
  const char *desc; // Step description
  uint8_t spacing;  // Seek spacing
} Step;

typedef struct
{
  uint8_t mode;     // Combination of RDS_* values
  const char *desc; // Mode description
} RDSMode;

//
// Global Variables
//

extern Band bands[];
extern Memory memories[];
extern const UTCOffset utcOffsets[];
extern const char *bandModeDesc[];
extern const FMRegion fmRegions[];
extern int bandIdx;

// These are menu commands
static inline bool isMenuMode(uint16_t cmd)
{
  return((cmd>=CMD_BAND) && (cmd<CMD_SETTINGS));
}

// These are settings
static inline bool isSettingsMode(uint16_t cmd)
{
  return((cmd>=CMD_SETTINGS) && (cmd<CMD_ABOUT));
}

uint8_t seekMode(bool toggle = false);
void drawSideBar(uint16_t cmd, int x, int y, int sx);
bool doSideBar(uint16_t cmd, int16_t enc, int16_t enca);
void doSelectDigit(int16_t enc);
bool clickHandler(uint16_t cmd, bool shortPress);
void selectBand(uint8_t idx, bool drawLoadingSSB = true);
// Tune to an arbitrary frequency/mode/band triple. False if the triple does not
// describe a reachable dial position. (Defined in Menu.cpp; declared here so
// AirTime's net list can use the same path the memory slots do.)
bool tuneToMemory(const Memory *memory);
int getTotalBands();
int getTotalModes();
int getTotalMemories();
Band *getCurrentBand();
uint8_t getFreqInputPos();
int getFreqInputStep();
const Step *getCurrentStep();
const Bandwidth *getCurrentBandwidth();
uint8_t getRDSMode();

int getCurrentUTCOffset();
int getTotalUTCOffsets();
int getTotalFmRegions();
int getTotalBleModes();

void doSoftMute(int16_t enc);
void doAgc(int16_t enc);
void doAvc(int16_t enc);
void doFmRegion(int16_t enc);
void doBandwidth(int16_t enc);
void doVolume(int16_t enc);
void doBrt(int16_t enc);
void doCal(int16_t enc);
void doStep(int16_t enc);
void doMode(int16_t enc);
void doBand(int16_t enc);

#endif // MENU_H

#ifdef AIRTIME
// AirTime settings, implemented in AirTimeMode.cpp. Declared here so Menu.cpp
// can drive them without taking a dependency on the AirTime library headers —
// the menu owns the turning and the drawing, AirTime owns the meaning.
int atZoneCount();  const char *atZoneName(int i);  int atZoneIdx();  void atSetZoneIdx(int i);
int atBandCount();  const char *atBandName(int i);  int atBandIdx();  void atSetBandIdx(int i);
int atHfCount();    const char *atHfName(int i);    int atHfIdx();    void atSetHfIdx(int i);
int atModeCount();  const char *atModeName(int i);  int atModeIdx();  void atSetModeIdx(int i);
// Mode and HF Listen scroll a SELECTION and commit on click — they are lists
// of actions, and firing them per encoder detent let a browse of the menu
// switch modes and start surveys (a field-found bug, not a style choice).
int atModeSelIdx();  void atSetModeSel(int i);  void atModeSelReset();  void atModeCommit();
int atHfSelIdx();    void atSetHfSel(int i);    void atHfSelReset();    void atHfCommit();
const char *atNetDetail();
int atNetCount();   const char *atNetName(int i);   int atNetIdx();   void atSetNetIdx(int i);

// The app's state, pre-formatted for a screen. Both the TFT layout and the web
// status page render from this, so the two can never disagree about what the
// radio is doing — the phone in your hand and the panel on the bench are
// reading the same sentence.
struct AirTimeScreen {
  const char *clock;    // "23:31:26", or "--:--:--" before any fix
  const char *local;    // same instant in the operator's zone
  const char *zone;     // "CDT"
  const char *status;   // "+/-250 ms   RDS   sync 26s ago"
  const char *tuned;    // "FM 89.9 MHz" / "WWV 15000 kHz  LISTENING"
  const char *clients;  // "NTP: 2 clients"
  const char *net;      // what is on the air, or ""
  // Why it is still searching: "RSSI 14 SNR 2  no-sync 812  g0". Empty once
  // synced. The web page carries the same numbers, but what actually comes
  // back from the field is a PHOTOGRAPH OF THE SCREEN, so the screen has to
  // be able to answer the question on its own.
  const char *diag;
  bool synced;          // false => clock drawn in the warning colour
  bool valid;           // false => no time at all yet
};
void airtimeScreen(AirTimeScreen *out);

// The status page on the device's own access point. Starts and stops with the
// AP, so it is up exactly when WiFi is — which is exactly when the WWV sampler
// is not (PLAN.md §2).
void airtimeWebService(bool wifi_up);
// Tune the receiver to a net, taking the dial from AirTime to do it.
// False if the net's frequency falls outside every band in bands[].
bool atTuneNet(int i);
bool airtimeRadioMode();
// True while AirTime is driving the tuner rather than the operator —
// i.e. clock mode, whether harvesting RDS or listening for WWV. Callers
// use it to keep expensive or meaningless work off a dial they do not own.
bool airtimeOwnsDial();
// The About page for this build, as ready-to-draw lines. Assembled here for
// the same reason AirTimeScreen is: About.cpp needs no AirTime headers, and
// there is exactly one place that decides what the device says about itself.
// Returns how many of `out` were filled.
int airtimeAboutLines(const char *out[], int max);
// CW copy: a third mode, in which the access point is DOWN (the audio tap and
// the WiFi radio cannot both be live) and the panel becomes a text terminal.
bool airtimeCwMode();
// Waterfall: the audio passband as spectrum + history. Same tap, same §2
// price as CW copy — the access point is down while it is on screen.
bool airtimeSpectrumMode();
#define AT_SPECTRUM_BINS 64
// Copy the newest spectrum frame; returns its frame counter (0 = none yet).
uint32_t atSpectrumCopy(float out[AT_SPECTRUM_BINS]);
// Wipe every learned/persisted AirTime byte and reboot. See NvsTimeStore.
void atResetLearning();
const char *atCwText();
int atCwWpm();
bool atCwKeyDown();
int atCwLevelPct();
#endif
