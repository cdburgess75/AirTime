//
// AirTime — Milestone 1 RDS station survey probe.
//
// Compiled out unless -DAIRTIME_RDS_SURVEY is passed (tools/build_fw.sh survey).
// Runbook: docs/MILESTONE1.md.
//
// What it measures, and why: §4 makes multi-station RDS voting mandatory, and
// STATUS.md records that a station with a *systematic* CT offset cannot be
// voted or weighted away — the arbiter caveat hinges on whether real stations
// are actually biased. This probe produces that number per station. The offset
// itself is measured by the laptop: every `SVY ct` line is printed the moment
// the group leaves the chip FIFO, tools/survey_log.py stamps its arrival
// against the Mac's NTP-synced clock, and tools/survey_report.py subtracts the
// asserted minute. The deterministic 87.6 ms group-transmission time is
// removed in the report; what remains is station bias plus a few tens of ms
// of poll/serial jitter — noise well under the 220 ms-class bias we care about.
//
// Sequence: one pass over the US FM raster (87.9-107.9 MHz, 200 kHz) reading
// RSSI/SNR, then a round-robin dwell over every channel above threshold,
// forever. Dwells end early when they can: a station with no RDS sync after
// 12 s is skipped, and once a CT group is caught we linger only long enough
// to finish the PS name — CT comes once a minute, so there is nothing more
// to wait for. Run at least 30 minutes; each round adds CT samples and the
// report uses the median.

#ifdef AIRTIME_RDS_SURVEY

#ifdef AIRTIME
#error "AIRTIME and AIRTIME_RDS_SURVEY are mutually exclusive builds"
#endif

#include "Common.h"

#include <airtime_core.h>  // decodeRdsClockTime; umbrella include, see header

namespace {

// US channel raster in SI4735 FM units (10 kHz). EU would be 8750..10800/10.
constexpr int32_t kScanStart = 8790;
constexpr int32_t kScanEnd = 10790;
constexpr int32_t kScanStep = 20;

constexpr uint32_t kSettleMs = 180;        // RSSI settle after retune
constexpr uint8_t kMinSnr = 8;             // dwell-list threshold, dB
constexpr uint32_t kDwellMs = 75000;       // spans one minute boundary
constexpr uint32_t kNoSyncBailMs = 12000;  // no RDS sync at all -> skip
constexpr uint32_t kAfterCtLingerMs = 4000;
constexpr size_t kMaxStations = 48;

enum class St { Boot, Scan, DwellTune, Dwell };

St state = St::Boot;
int32_t scan_f = kScanStart;
uint32_t t_state = 0;

// When the list is full, a stronger late-dial station replaces the weakest
// listed one — a dense market must not silently truncate the top of the dial
// (observed in the field: n=32 filled by 101.1 MHz).
int32_t dwell[kMaxStations];
uint8_t dwell_snr[kMaxStations];
// Learned per station across rounds. Round 1 explores everything once; later
// rounds dwell only on stations that actually sent clock-time, so CT samples
// accumulate fast instead of re-waiting 75 s on every no-CT station. RDS CT
// is once a minute by the standard — one clean spanning dwell is decisive.
bool had_rds[kMaxStations];
bool had_ct[kMaxStations];
bool any_ct = false;
size_t dwell_n = 0;
size_t dwell_i = 0;
uint32_t round_n = 1;

// Per-dwell decode state
uint16_t cur_pi = 0;
char ps[9];
uint8_t ps_mask = 0;
bool ps_printed = false, sync_seen = false, ct_seen = false;
uint32_t t_ct = 0;

void tuneFm(int32_t f)
{
  rx.setFrequency((uint16_t)f);
  rx.RdsInit();
}

void enterFmMode()
{
  rx.setFM(6400, 10800, (uint16_t)kScanStart, 10);
  rx.setGpioCtl(1, 0, 0);
  rx.setGpio(0, 0, 0);            // FM antenna path (see useBand)
  rx.setRdsConfig(1, 2, 2, 2, 2);
}

void resetDwellDecode()
{
  cur_pi = 0;
  ps_mask = 0;
  ps_printed = false;
  sync_seen = false;
  ct_seen = false;
  memset(ps, ' ', 8);
  ps[8] = 0;
}

bool eligible(size_t i)
{
  if(round_n == 1) return true;   // explore the whole list once
  if(had_ct[i]) return true;      // then focus on stations that pay
  if(!any_ct) return had_rds[i];  // no CT anywhere yet: keep trying RDS ones
  return false;
}

void nextDwell()
{
  for(size_t hop = 0; hop <= dwell_n; hop++)
  {
    dwell_i++;
    if(dwell_i >= dwell_n)
    {
      dwell_i = 0;
      size_t ct_n = 0;
      for(size_t i = 0; i < dwell_n; i++) ct_n += had_ct[i] ? 1 : 0;
      Serial.printf("SVY round %lu done ct_stations=%u\n",
                    (unsigned long)round_n++, (unsigned)ct_n);
    }
    if(eligible(dwell_i)) break;
  }
  state = St::DwellTune;
}

void handleGroup(uint16_t w[4], uint8_t ble[4])
{
  // PI keys everything downstream; take it only from a clean-ish block A.
  if(ble[0] <= 1 && w[0] != 0 && w[0] != cur_pi)
  {
    cur_pi = w[0];
    Serial.printf("SVY pi f=%ld pi=%04X\n", (long)dwell[dwell_i], cur_pi);
  }

  const int type = airtime::rdsGroupType(w[1]);

  // Group 0 (A or B): PS name, two chars per segment.
  if(type == 0 && ble[1] <= 1 && ble[3] <= 1)
  {
    const int seg = w[1] & 0x03;
    ps[seg * 2] = (char)(w[3] >> 8);
    ps[seg * 2 + 1] = (char)(w[3] & 0xFF);
    ps_mask |= (uint8_t)(1 << seg);
    if(ps_mask == 0x0F && !ps_printed)
    {
      for(int i = 0; i < 8; i++)
        if(ps[i] < 0x20 || ps[i] > 0x7E) ps[i] = '?';
      Serial.printf("SVY ps f=%ld pi=%04X ps=\"%s\"\n", (long)dwell[dwell_i], cur_pi, ps);
      ps_printed = true;
    }
  }

  // Group 4A: clock-time. Print IMMEDIATELY — the host timestamp on this line
  // is the measurement. Log regardless of BLE (the field is in the line; the
  // report filters), so marginal stations are visible rather than absent.
  if(type == 4 && !airtime::rdsIsVersionB(w[1]))
  {
    airtime::RdsClockTime t;
    if(airtime::decodeRdsClockTime(w[0], w[1], w[2], w[3], &t))
    {
      Serial.printf(
          "SVY ct f=%ld pi=%04X utc=%lld ble=%u%u%u%u raw=%04X%04X%04X%04X dev_ms=%lu\n",
          (long)dwell[dwell_i], cur_pi, (long long)t.utc_epoch_s,
          ble[0], ble[1], ble[2], ble[3], w[0], w[1], w[2], w[3],
          (unsigned long)millis());
      ct_seen = true;
      t_ct = millis();
    }
  }
}

void pumpRds()
{
  uint16_t w[4];
  uint8_t ble[4];
  for(int i = 0; i < 4; i++)
  {
    rx.getRdsStatus(1, 0, 0);
    if(!rx.getRdsSync()) return;
    sync_seen = true;
    if(!(rx.getRdsReceived() || rx.getNumRdsFifoUsed() > 0)) return;
    rx.getRdsRawGroup(w, ble);
    handleGroup(w, ble);
    if(rx.getNumRdsFifoUsed() == 0) return;
  }
}

}  // namespace

void airtimeRdsSurvey()
{
  const uint32_t now = millis();

  switch(state)
  {
    case St::Boot:
      Serial.printf("SVY start raster=%ld..%ld/%ld snr_min=%u dwell_ms=%lu\n",
                    (long)kScanStart, (long)kScanEnd, (long)kScanStep,
                    (unsigned)kMinSnr, (unsigned long)kDwellMs);
      enterFmMode();
      scan_f = kScanStart;
      tuneFm(scan_f);
      t_state = now;
      state = St::Scan;
      break;

    case St::Scan:
    {
      if(now - t_state < kSettleMs) break;
      rx.getCurrentReceivedSignalQuality(0);
      const uint8_t r = rx.getCurrentRSSI(), s = rx.getCurrentSNR();
      if(s >= kMinSnr)
      {
        if(dwell_n < kMaxStations)
        {
          dwell_snr[dwell_n] = s;
          dwell[dwell_n++] = scan_f;
          Serial.printf("SVY sig f=%ld rssi=%u snr=%u\n", (long)scan_f, r, s);
        }
        else
        {
          size_t w = 0;
          for(size_t i = 1; i < dwell_n; i++)
            if(dwell_snr[i] < dwell_snr[w]) w = i;
          if(s > dwell_snr[w])
          {
            Serial.printf("SVY sig f=%ld rssi=%u snr=%u (replaces f=%ld)\n",
                          (long)scan_f, r, s, (long)dwell[w]);
            dwell[w] = scan_f;
            dwell_snr[w] = s;
          }
        }
      }
      scan_f += kScanStep;
      if(scan_f > kScanEnd)
      {
        Serial.printf("SVY scan_done n=%u\n", (unsigned)dwell_n);
        if(dwell_n == 0)
        {
          // Nothing receivable; rescan (antenna unplugged? band dead?).
          scan_f = kScanStart;
        }
        else
        {
          dwell_i = 0;
          state = St::DwellTune;
          break;
        }
      }
      tuneFm(scan_f);
      t_state = now;
      break;
    }

    case St::DwellTune:
      tuneFm(dwell[dwell_i]);
      resetDwellDecode();
      Serial.printf("SVY tune f=%ld\n", (long)dwell[dwell_i]);
      t_state = now;
      state = St::Dwell;
      break;

    case St::Dwell:
      pumpRds();
      if(ct_seen && (ps_printed || now - t_ct > kAfterCtLingerMs))
      {
        nextDwell();  // CT caught; the next one is a minute away
      }
      else if(!sync_seen && now - t_state > kNoSyncBailMs)
      {
        Serial.printf("SVY nosync f=%ld\n", (long)dwell[dwell_i]);
        nextDwell();
      }
      else if(now - t_state > kDwellMs)
      {
        Serial.printf("SVY noct f=%ld pi=%04X\n", (long)dwell[dwell_i], cur_pi);
        nextDwell();
      }
      break;
  }
}

#endif  // AIRTIME_RDS_SURVEY
