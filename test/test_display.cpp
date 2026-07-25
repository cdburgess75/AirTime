#include <cstring>

#include "airtime/display.h"
#include "test_framework.h"

using namespace airtime;

namespace {
bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
constexpr int64_t kS = 1000000;
}  // namespace

// The §5 headline: "14:22:07 UTC"
AT_TEST(disp_utc_line) {
  DisplayState st;
  st.clock_valid = true;
  st.utc_us = (14 * 3600 + 22 * 60 + 7) * kS;
  char buf[32];
  formatUtcLine(st, buf, sizeof(buf));
  AT_CHECK(eq(buf, "14:22:07 UTC"));
}

// Before any time at all, show nothing rather than something wrong.
AT_TEST(disp_utc_line_invalid) {
  DisplayState st;
  st.clock_valid = false;
  char buf[32];
  formatUtcLine(st, buf, sizeof(buf));
  AT_CHECK(eq(buf, "--:--:-- UTC"));
}

// The §5 status line, verbatim.
AT_TEST(disp_status_line_synced) {
  DisplayState st;
  st.clock_valid = true;
  st.synced = true;
  st.ever_synced = true;
  st.uncertainty_us = 60000;
  st.since_sync_us = 3 * 3600 * kS;
  st.sources = kSrcRds | kSrcWwv;
  st.ntp_clients = 2;

  char buf[128];
  formatStatusLine(st, buf, sizeof(buf));
  AT_CHECK(eq(buf, "±60 ms · RDS+WWV · sync 3h ago · NTP: 2 clients"));
}

// Stale sync says so plainly (§5) instead of showing confident error bars.
AT_TEST(disp_status_line_unsynced) {
  DisplayState st;
  st.clock_valid = true;
  st.synced = false;
  st.ever_synced = true;
  st.since_sync_us = 26 * 3600 * kS;
  st.ntp_clients = 1;

  char buf[128];
  formatStatusLine(st, buf, sizeof(buf));
  AT_CHECK(std::strstr(buf, "UNSYNCED — last-known + drift") != nullptr);
  AT_CHECK(std::strstr(buf, "1d ago") != nullptr);
}

AT_TEST(disp_uncertainty_scales) {
  char b[24];
  formatUncertainty(500, b, sizeof(b));       AT_CHECK(eq(b, "±500 us"));
  formatUncertainty(60000, b, sizeof(b));     AT_CHECK(eq(b, "±60 ms"));
  formatUncertainty(1200000, b, sizeof(b));   AT_CHECK(eq(b, "±1.2 s"));
  formatUncertainty(180000000, b, sizeof(b)); AT_CHECK(eq(b, "±3 min"));
  formatUncertainty(-60000, b, sizeof(b));    AT_CHECK(eq(b, "±60 ms"));
}

AT_TEST(disp_age_scales) {
  char b[16];
  formatAge(45 * kS, b, sizeof(b));        AT_CHECK(eq(b, "45s"));
  formatAge(12 * 60 * kS, b, sizeof(b));   AT_CHECK(eq(b, "12m"));
  formatAge(3 * 3600 * kS, b, sizeof(b));  AT_CHECK(eq(b, "3h"));
  formatAge(50 * 3600 * kS, b, sizeof(b)); AT_CHECK(eq(b, "2d"));
}

AT_TEST(disp_sources_combine) {
  char b[24];
  formatSources(kSrcRds, b, sizeof(b));            AT_CHECK(eq(b, "RDS"));
  formatSources(kSrcWwv, b, sizeof(b));            AT_CHECK(eq(b, "WWV"));
  formatSources(kSrcRds | kSrcWwv, b, sizeof(b));  AT_CHECK(eq(b, "RDS+WWV"));
  formatSources(0, b, sizeof(b));                  AT_CHECK(eq(b, "none"));
}
