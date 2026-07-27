#pragma once
//
// IRdsSource over the SI4732's RDS decoder — PLAN.md §4.
//
// The chip does the heavy lifting: block sync, error correction, and a FIFO of
// decoded groups. This adapter's whole job is to poll that FIFO at a sane rate,
// keep only groups whose blocks survived error correction cleanly, timestamp
// them, and hand them to the core. Clock-time decoding, station voting and the
// arbiter are host-tested code in airtime_core.
//
// ── Design notes ────────────────────────────────────────────────────────────
//
// **Chip access is a seam, not a dependency.** The SI4735 library keeps its
// status struct protected, and the host firmware already owns the one radio
// instance (ats-mini's `SI4735_fixed rx`). So the adapter takes two function
// pointers — tune and read — implemented in ~15 lines of sketch glue where
// that instance is visible. Same pattern as the WWV sampler's TuneFn.
//
// **Poll rate is capped.** Every FM_RDS_STATUS command costs an I2C round trip
// plus the library's fixed 550 µs settle. RDS delivers ~11.4 groups/s, so
// polling every main-loop pass (kHz) would saturate the bus for nothing. The
// default 40 ms interval samples faster than groups arrive, with the chip's
// FIFO absorbing any burst; the resulting timestamp quantisation is well inside
// RDS clock-time's ±250 ms error budget.
//
// **Arrival timestamps are corrected for group length.** An RDS group is 104
// bits at 1187.5 bps — 87.6 ms on the air — and IEC 62106 aligns the minute
// edge with the *start* of the 4A group's transmission. The chip can only hand
// over a group after receiving all of it, so raw arrival stamps run one group
// late, systematically. That is exactly the class of per-source bias the
// arbiter cannot vote away (see STATUS.md), so the deterministic part is
// removed here: stamps are backdated by group_latency_us.
//
// **Error gating is stricter than the chip's.** ats-mini configures the chip
// to deliver groups with up to 5 corrected bits per block (fine for scrolling
// text). A clock-time group steers a clock, and its PI code keys the
// multi-station voter — a miscorrected block can move time or credit the wrong
// station. Groups are dropped unless every block decoded with at most 1-2
// corrected bits (BLE <= 1).

#include <cstddef>
#include <cstdint>

#include <airtime/hal.h>

namespace airtime_esp32 {

// Return contract for RdsChipOps::read — the refusals are distinct on purpose,
// because each names a different broken thing: NotFm means the chip is not
// where the software believes; NoSync means it is there but the RDS decoder
// has nothing to lock to (config or propagation); 0 means locked but the FIFO
// is empty right now, which is the only healthy kind of nothing.
constexpr int kRdsReadNotFm = -2;
constexpr int kRdsReadNoSync = -1;

// Implemented by the integration layer, where the SI4735 instance lives.
struct RdsChipOps {
  // Optional: current RSSI on the tuned frequency, 0..127. Leave null when the
  // host cannot answer; the survey then falls back to dwelling everywhere.
  int (*rssi)(void* ctx) = nullptr;
  // Tune the receiver to an FM frequency in SI4735 native FM units (10 kHz —
  // 9110 is 91.1 MHz), switching it into FM mode if needed.
  void (*tune)(int32_t khz10, void* ctx);

  // Issue one FM_RDS_STATUS (INTACK=1, MTFIFO=0, STATUSONLY=0) and report:
  //   < 0  unusable — not in FM mode, or no RDS sync; out params untouched
  //     0  no new group since the last call
  //     1  one group returned in w[] (A,B,C,D) and ble[] (per-block errors 0-3)
  //     2  as 1, and the chip FIFO still holds more — poll again immediately
  // Returning <0 when the chip is not in FM mode is REQUIRED: the SI4735
  // library silently skips the I2C transaction outside FM, leaving a stale
  // status struct that must not be re-read as fresh data.
  int (*read)(uint16_t w[4], uint8_t ble[4], void* ctx);

  void* ctx = nullptr;
};

struct RdsSourceConfig {
  int64_t poll_interval_us = 40000;  // chip poll cap; > group rate, << CT budget
  int64_t group_latency_us = 87600;  // 104 bits / 1187.5 bps: stamp -> group start
  uint8_t max_block_errors = 1;      // per-block BLE ceiling (0-3); see above
  int max_reads_per_poll = 4;        // FIFO drain bound per poll() call
};

class Esp32RdsSource : public airtime::IRdsSource {
 public:
  Esp32RdsSource() = default;

  void begin(const RdsSourceConfig& cfg, const RdsChipOps& ops,
             airtime::IMonotonicClock* clock) {
    cfg_ = cfg;
    ops_ = ops;
    clock_ = clock;
  }

  // --- IRdsSource ------------------------------------------------------------
  void tuneKhz(int32_t khz) override {
    tuned_khz_ = khz;
    next_poll_us_ = 0;  // a fresh station may speak sooner than the cap
    if (ops_.tune != nullptr) ops_.tune(khz, ops_.ctx);
  }

  int32_t tunedKhz() const override { return tuned_khz_; }

  // Signal strength, for the dial survey. Supplied by the host firmware
  // through RdsChipOps::rssi because reading it means talking to the tuner,
  // which is the one thing this adapter does not own.
  int signalStrength() const override {
    return ops_.rssi != nullptr ? ops_.rssi(ops_.ctx) : -1;
  }

  bool poll(airtime::RdsGroup* out) override;

  // --- Diagnostics -----------------------------------------------------------
  uint32_t groupsAccepted() const { return accepted_; }
  uint32_t groupsRejected() const { return rejected_; }

  // The pipeline, stage by stage. Field-earned: a device sat at "0 groups
  // used" with a pegged S-meter, and nothing could say whether reads were
  // refused (chip not in FM), the decoder had no sync (config/signal), the
  // FIFO was simply empty, or the chip was never asked. One counter per
  // verdict of read(); together they name the failing stage outright.
  uint32_t pollsNotFm() const { return not_fm_; }
  uint32_t pollsNoSync() const { return no_sync_; }
  uint32_t pollsEmpty() const { return empty_; }

 private:
  RdsSourceConfig cfg_;
  RdsChipOps ops_{};
  airtime::IMonotonicClock* clock_ = nullptr;

  uint32_t not_fm_ = 0;
  uint32_t no_sync_ = 0;
  uint32_t empty_ = 0;

  int32_t tuned_khz_ = 0;
  int64_t next_poll_us_ = 0;
  uint32_t accepted_ = 0;
  uint32_t rejected_ = 0;
};

}  // namespace airtime_esp32
