#include <cstdint>
#include <cstring>

#include "airtime/source_table.h"
#include "test_framework.h"

using namespace airtime;

namespace {
constexpr int64_t kMs = 1000;
}  // namespace

AT_TEST(source_new_row_is_unknown) {
  SourceTable t;
  const SourceRow* r = t.fm(10470);
  AT_CHECK(r != nullptr);
  AT_CHECK(t.rating(*r) == Rating::Unknown);
  AT_CHECK(t.fm(10470) == r);          // one row per source
  AT_CHECK_EQ(t.count(), static_cast<std::size_t>(1));
}

// One source may set the clock — but it has only spoken, not been checked.
AT_TEST(source_delivered_time_is_yellow_until_confirmed) {
  SourceTable t;
  SourceRow* r = t.fm(10470);
  t.noteTime(r, 0, /*confirmable=*/false);
  AT_CHECK(t.rating(*r) == Rating::Yellow);
  t.noteTime(r, 0, false);
  AT_CHECK(t.rating(*r) == Rating::Yellow);   // repeating itself confirms nothing
}

AT_TEST(source_confirmed_by_a_different_source_is_green) {
  SourceTable t;
  SourceRow* r = t.fm(10470);
  t.noteTime(r, 0, false);
  t.noteTime(r, 180 * kMs, /*confirmable=*/true);   // within 400 ms
  AT_CHECK(t.rating(*r) == Rating::Green);
}

// Wrong against a confirmed clock is Red — and the station comes back the
// moment it delivers good time again.
AT_TEST(source_wrong_against_a_confirmed_clock_is_red_and_recovers) {
  SourceTable t;
  SourceRow* r = t.fm(9110);
  t.noteTime(r, 1093610205 * kMs, true);   // the 12.6-day date seen in the field
  AT_CHECK(t.rating(*r) == Rating::Red);
  t.noteTime(r, 20 * kMs, true);
  AT_CHECK(t.rating(*r) == Rating::Green);
}

// Sloppy is not wrong: between agreeing and the bias cap stays Yellow.
AT_TEST(source_sloppy_green_drops_to_yellow_not_red) {
  SourceTable t;
  SourceRow* r = t.fm(10750);
  t.noteTime(r, 50 * kMs, true);
  AT_CHECK(t.rating(*r) == Rating::Green);
  t.noteTime(r, 900 * kMs, true);
  AT_CHECK(t.rating(*r) == Rating::Yellow);
}

AT_TEST(source_silent_dwells_turn_red_and_time_recovers) {
  SourceTable t;
  SourceRow* r = t.fm(8930);
  t.noteDwellEnd(r, false);
  AT_CHECK(t.rating(*r) == Rating::Unknown);   // one dwell is not a verdict
  t.noteDwellEnd(r, false);
  AT_CHECK(t.rating(*r) == Rating::Red);
  t.noteTime(r, 0, false);
  AT_CHECK(t.rating(*r) == Rating::Yellow);
}

AT_TEST(source_names_fit_the_zoom_box) {
  SourceTable t;
  SourceRow* fm = t.fm(10470);
  SourceRow* w25 = t.wwv(2500);
  SourceRow* w15 = t.wwv(15000);
  char b[32];
  SourceTable::name(*fm, Rating::Green, b, sizeof(b));
  AT_CHECK(std::strcmp(b, "G 104.7") == 0);
  SourceTable::name(*w25, Rating::Unknown, b, sizeof(b));
  AT_CHECK(std::strcmp(b, "? WWV 2.5") == 0);
  SourceTable::name(*w15, Rating::Red, b, sizeof(b));
  AT_CHECK(std::strcmp(b, "R WWV 15") == 0);
  SourceRow wide;
  wide.freq = 10790;
  SourceTable::name(wide, Rating::Yellow, b, sizeof(b));
  AT_CHECK(std::strlen(b) <= 12);
}

AT_TEST(source_table_round_trips) {
  SourceTable a;
  SourceRow* fm = a.fm(10470);
  fm->pi = 0x6E47;
  a.noteTime(fm, 0, false);
  a.noteTime(fm, -120 * kMs, true);
  SourceRow* w = a.wwv(15000);
  a.noteDwellEnd(w, false);
  a.noteDwellEnd(w, false);

  uint8_t blob[SourceTable::kBlobMax];
  const std::size_t n = a.encode(blob, sizeof(blob));
  AT_CHECK(n > 0);

  SourceTable b;
  AT_CHECK(b.decode(blob, n));
  AT_CHECK_EQ(b.count(), a.count());
  const SourceRow* bf = b.find(SourceKind::Fm, 10470);
  AT_CHECK(bf != nullptr && bf->pi == 0x6E47);
  AT_CHECK(b.rating(*bf) == Rating::Green);
  AT_CHECK_EQ(static_cast<int>(bf->err_ms), -120);
  const SourceRow* bw = b.find(SourceKind::Wwv, 15000);
  AT_CHECK(bw != nullptr && b.rating(*bw) == Rating::Red);
}

// A rating read back from flash must be verified whole: a false Green is worse
// than starting over.
AT_TEST(source_table_rejects_anything_it_cannot_verify) {
  SourceTable a;
  a.noteTime(a.fm(10470), 0, true);
  uint8_t blob[SourceTable::kBlobMax];
  const std::size_t n = a.encode(blob, sizeof(blob));

  SourceTable b;
  AT_CHECK(!b.decode(blob, n - 1));        // short
  uint8_t bad[SourceTable::kBlobMax];
  std::memcpy(bad, blob, n);
  bad[0] = 9;                               // unknown version
  AT_CHECK(!b.decode(bad, n));
  std::memcpy(bad, blob, n);
  bad[2] = 7;                               // unknown kind
  AT_CHECK(!b.decode(bad, n));
  AT_CHECK_EQ(b.count(), static_cast<std::size_t>(0));   // untouched throughout
}

using namespace airtime;

AT_TEST(source_table_clear_fm_keeps_wwv_and_put_row_restores) {
  SourceTable t;
  t.fm(10610)->pi = 0x829D;
  t.wwv(10000)->heard = 2;
  SourceRow saved = *t.fm(9230);
  saved.pi = 0x986D;
  saved.heard = 4;
  saved.wrong = true;
  t.clearFm();
  AT_CHECK(t.find(SourceKind::Fm, 10610) == nullptr);
  AT_CHECK(t.find(SourceKind::Wwv, 10000) != nullptr);
  t.putRow(saved);
  const SourceRow* r = t.find(SourceKind::Fm, 9230);
  AT_CHECK(r != nullptr && r->wrong && r->pi == 0x986D && r->heard == 4);
}
