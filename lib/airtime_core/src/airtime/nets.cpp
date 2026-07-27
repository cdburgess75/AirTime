#include "nets.h"

namespace airtime {

namespace {

constexpr int kMinutesPerDay = 1440;
constexpr int kMinutesPerWeek = kMinutesPerDay * 7;

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// Minutes since the start of the UTC week (Sunday 00:00), 0..10079.
int weekMinute(int64_t utc_s) {
  const int64_t day = floorDiv(utc_s, 86400);
  const int wd = static_cast<int>(((day % 7) + 7 + 4) % 7);  // 1970-01-01 = Thu
  const int min_of_day =
      static_cast<int>(floorDiv(utc_s, 60) - day * 1440);
  return wd * kMinutesPerDay + min_of_day;
}

// How far into a net's session `now_wm` is, or -1 if it is not running.
//
// A session is checked against every day its mask names, INCLUDING the day
// before: a net that starts at 2330 and runs an hour is still on the air at
// 0015 the next morning, and forgetting that would leave the display saying
// nothing is on while the net is mid-sentence.
int sessionElapsed(const HamNet& n, int now_wm) {
  for (int d = 0; d < 7; ++d) {
    if ((n.days & (1 << d)) == 0) continue;
    const int start = d * kMinutesPerDay + n.start_min;
    int delta = now_wm - start;
    if (delta < 0) delta += kMinutesPerWeek;   // started before the week rolled
    if (delta < n.duration_min) return delta;
  }
  return -1;
}

}  // namespace

int utcWeekday(int64_t utc_s) {
  const int64_t day = floorDiv(utc_s, 86400);
  return static_cast<int>(((day % 7) + 7 + 4) % 7);
}

int netActiveAt(const HamNet* nets, std::size_t count, int64_t utc_s) {
  if (nets == nullptr) return -1;
  const int now_wm = weekMinute(utc_s);
  for (std::size_t i = 0; i < count; ++i) {
    if (nets[i].duration_min <= 0 || nets[i].days == 0) continue;
    if (sessionElapsed(nets[i], now_wm) >= 0) return static_cast<int>(i);
  }
  return -1;
}

int netNextAt(const HamNet* nets, std::size_t count, int64_t utc_s,
              int* minutes_until) {
  if (nets == nullptr || count == 0) return -1;

  const int now_wm = weekMinute(utc_s);
  int best = -1;
  int best_wait = kMinutesPerWeek + 1;

  for (std::size_t i = 0; i < count; ++i) {
    const HamNet& n = nets[i];
    if (n.duration_min <= 0 || n.days == 0) continue;

    // On the air now counts as zero wait — the answer to "what is next" when
    // something is already running is that thing.
    if (sessionElapsed(n, now_wm) >= 0) {
      if (0 < best_wait) { best_wait = 0; best = static_cast<int>(i); }
      continue;
    }

    for (int d = 0; d < 7; ++d) {
      if ((n.days & (1 << d)) == 0) continue;
      const int start = d * kMinutesPerDay + n.start_min;
      int wait = start - now_wm;
      if (wait < 0) wait += kMinutesPerWeek;   // next week's occurrence
      if (wait < best_wait) {
        best_wait = wait;
        best = static_cast<int>(i);
      }
    }
  }

  if (best >= 0 && minutes_until != nullptr) *minutes_until = best_wait;
  return best;
}

}  // namespace airtime
