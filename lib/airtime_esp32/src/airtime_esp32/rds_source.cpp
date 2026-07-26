#include "rds_source.h"

namespace airtime_esp32 {

bool Esp32RdsSource::poll(airtime::RdsGroup* out) {
  if (ops_.read == nullptr || clock_ == nullptr) return false;

  const int64_t now = clock_->nowUs();
  if (now < next_poll_us_) return false;
  next_poll_us_ = now + cfg_.poll_interval_us;

  // Drain the chip FIFO, bounded. poll() returns at most one group per call
  // (the interface is one-at-a-time), but the app calls it in a while-loop, so
  // anything left in the chip is picked up on the very next iteration — the
  // interval gate above only applies once the chip reports it is drained.
  uint16_t w[4];
  uint8_t ble[4];
  for (int i = 0; i < cfg_.max_reads_per_poll; ++i) {
    const int r = ops_.read(w, ble, ops_.ctx);
    if (r <= 0) return false;

    if (r >= 2) next_poll_us_ = now;  // more waiting: let the next call in now

    if (ble[0] > cfg_.max_block_errors || ble[1] > cfg_.max_block_errors ||
        ble[2] > cfg_.max_block_errors || ble[3] > cfg_.max_block_errors) {
      ++rejected_;
      continue;  // marginal group; with more in the FIFO, try the next one
    }

    ++accepted_;
    out->a = w[0];
    out->b = w[1];
    out->c = w[2];
    out->d = w[3];
    // Backdate to the start of the group's transmission (see header).
    out->mono_us = now - cfg_.group_latency_us;
    return true;
  }
  return false;
}

}  // namespace airtime_esp32
