#include "display.h"

#include <cstdio>

namespace airtime {

uint8_t sourceBit(Source s) {
  switch (s) {
    case Source::Rds: return kSrcRds;
    case Source::Wwv: return kSrcWwv;
    case Source::Manual: return kSrcManual;
    case Source::None: break;
  }
  return 0;
}

void formatUncertainty(int64_t us, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;
  if (us < 0) us = -us;
  if (us < 1000) {
    std::snprintf(buf, n, "±%lld us", static_cast<long long>(us));
  } else if (us < 1000000) {
    std::snprintf(buf, n, "±%lld ms", static_cast<long long>(us / 1000));
  } else if (us < 60000000) {
    std::snprintf(buf, n, "±%.1f s", static_cast<double>(us) / 1e6);
  } else {
    std::snprintf(buf, n, "±%lld min", static_cast<long long>(us / 60000000));
  }
}

void formatAge(int64_t us, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;
  if (us < 0) us = 0;
  const long long s = static_cast<long long>(us / 1000000);
  if (s < 60) {
    std::snprintf(buf, n, "%llds", s);
  } else if (s < 3600) {
    std::snprintf(buf, n, "%lldm", s / 60);
  } else if (s < 86400) {
    std::snprintf(buf, n, "%lldh", s / 3600);
  } else {
    std::snprintf(buf, n, "%lldd", s / 86400);
  }
}

void formatSources(uint8_t mask, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;
  buf[0] = '\0';
  std::size_t used = 0;
  const struct { uint8_t bit; const char* name; } kAll[] = {
      {kSrcRds, "RDS"}, {kSrcWwv, "WWV"}, {kSrcManual, "MAN"}};
  for (const auto& e : kAll) {
    if ((mask & e.bit) == 0) continue;
    const int w = std::snprintf(buf + used, n - used, "%s%s",
                                used != 0 ? "+" : "", e.name);
    if (w <= 0 || static_cast<std::size_t>(w) >= n - used) return;
    used += static_cast<std::size_t>(w);
  }
  if (used == 0) std::snprintf(buf, n, "none");
}

void formatUtcLine(const DisplayState& st, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;
  if (!st.clock_valid) {
    std::snprintf(buf, n, "--:--:-- UTC");
    return;
  }
  int64_t sod = (st.utc_us / 1000000) % 86400;  // seconds of day
  if (sod < 0) sod += 86400;
  std::snprintf(buf, n, "%02lld:%02lld:%02lld UTC",
                static_cast<long long>(sod / 3600),
                static_cast<long long>((sod / 60) % 60),
                static_cast<long long>(sod % 60));
}

void formatStatusLine(const DisplayState& st, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;

  char srcs[24];
  formatSources(st.sources, srcs, sizeof(srcs));

  if (!st.clock_valid) {
    std::snprintf(buf, n, "NO TIME · acquiring · NTP: %d clients",
                  st.ntp_clients);
    return;
  }

  if (!st.synced) {
    // §5: be blunt rather than show a confident-looking wrong time.
    char age[16];
    formatAge(st.since_sync_us, age, sizeof(age));
    if (st.ever_synced) {
      std::snprintf(buf, n,
                    "UNSYNCED — last-known + drift · %s ago · NTP: %d clients",
                    age, st.ntp_clients);
    } else {
      std::snprintf(buf, n,
                    "UNSYNCED — last-known + drift · NTP: %d clients",
                    st.ntp_clients);
    }
    return;
  }

  char unc[24], age[16];
  formatUncertainty(st.uncertainty_us, unc, sizeof(unc));
  formatAge(st.since_sync_us, age, sizeof(age));
  std::snprintf(buf, n, "%s · %s · sync %s ago · NTP: %d clients",
                unc, srcs, age, st.ntp_clients);
}

}  // namespace airtime
