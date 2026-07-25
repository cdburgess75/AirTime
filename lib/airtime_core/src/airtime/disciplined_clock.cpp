#include "disciplined_clock.h"

#include <cmath>

namespace airtime {

DisciplinedClock::DisciplinedClock(int64_t max_slew_ppm)
    : max_slew_ppm_(max_slew_ppm) {}

int64_t DisciplinedClock::slewInjected(int64_t mono_us) const {
  if (slew_dur_ <= 0) return 0;
  if (mono_us <= slew_start_mono_) return 0;
  const int64_t el = mono_us - slew_start_mono_;
  if (el >= slew_dur_) return slew_offset_;
  return static_cast<int64_t>(llround(static_cast<double>(slew_offset_) *
                                      static_cast<double>(el) /
                                      static_cast<double>(slew_dur_)));
}

int64_t DisciplinedClock::utcAt(int64_t mono_us) const {
  if (!set_) return 0;
  const int64_t dt = mono_us - base_mono_;
  const double rated = static_cast<double>(dt) * (1.0 + rate_ppm_ / 1e6);
  return base_utc_ + static_cast<int64_t>(llround(rated)) + slewInjected(mono_us);
}

int64_t DisciplinedClock::consolidate(int64_t mono_us) {
  const int64_t inj = slewInjected(mono_us);
  const int64_t u = utcAt(mono_us);
  total_injected_ += inj;
  const int64_t remaining = slew_offset_ - inj;
  base_utc_ = u;
  base_mono_ = mono_us;
  slew_offset_ = 0;
  slew_dur_ = 0;
  slew_start_mono_ = mono_us;
  return remaining;
}

void DisciplinedClock::startSlew(int64_t mono_us, int64_t total_offset_us) {
  slew_start_mono_ = mono_us;
  if (total_offset_us == 0) {
    slew_offset_ = 0;
    slew_dur_ = 0;
    return;
  }
  const int64_t mag = total_offset_us < 0 ? -total_offset_us : total_offset_us;
  const int64_t dur =
      max_slew_ppm_ > 0
          ? static_cast<int64_t>(llround(static_cast<double>(mag) * 1e6 /
                                         static_cast<double>(max_slew_ppm_)))
          : 0;
  if (dur <= 0) {  // instant application
    base_utc_ += total_offset_us;
    total_injected_ += total_offset_us;
    slew_offset_ = 0;
    slew_dur_ = 0;
    return;
  }
  slew_offset_ = total_offset_us;
  slew_dur_ = dur;
}

void DisciplinedClock::set(int64_t mono_us, int64_t utc_us) {
  base_mono_ = mono_us;
  base_utc_ = utc_us;
  slew_offset_ = 0;
  slew_dur_ = 0;
  slew_start_mono_ = mono_us;
  set_ = true;  // rate_ppm_ and total_injected_ are intentionally preserved
}

void DisciplinedClock::steer(int64_t mono_us, int64_t offset_us) {
  if (!set_) return;
  const int64_t rem = consolidate(mono_us);
  startSlew(mono_us, rem + offset_us);
}

void DisciplinedClock::setRatePpm(int64_t mono_us, double ppm) {
  if (!set_) {
    rate_ppm_ = ppm;
    return;
  }
  const int64_t rem = consolidate(mono_us);
  rate_ppm_ = ppm;
  startSlew(mono_us, rem);
}

void DisciplinedClock::consolidateTo(int64_t mono_us) {
  if (!set_) {
    base_mono_ = mono_us;
    return;
  }
  const int64_t rem = consolidate(mono_us);
  startSlew(mono_us, rem);
}

}  // namespace airtime
