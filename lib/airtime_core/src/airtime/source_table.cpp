#include "source_table.h"

#include <cstdio>

namespace airtime {

namespace {

constexpr uint8_t kVersion = 1;
constexpr std::size_t kRowBytes = 16;

inline int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

void put16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v & 0xFF);
  *p++ = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t*& p, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  for (int s = 0; s < 32; s += 8) *p++ = static_cast<uint8_t>((u >> s) & 0xFF);
}
uint16_t get16(const uint8_t*& p) {
  const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  return v;
}
int32_t get32(const uint8_t*& p) {
  const uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
  p += 4;
  return static_cast<int32_t>(u);
}

}  // namespace

SourceRow* SourceTable::rowFor(SourceKind k, int32_t freq) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].kind == k && rows_[i].freq == freq) return &rows_[i];
  }
  if (count_ >= kMaxRows) return nullptr;
  SourceRow& r = rows_[count_++];
  r = SourceRow{};
  r.kind = k;
  r.freq = freq;
  return &r;
}

const SourceRow* SourceTable::find(SourceKind k, int32_t freq) const {
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].kind == k && rows_[i].freq == freq) return &rows_[i];
  }
  return nullptr;
}

void SourceTable::clearFm() {
  std::size_t w = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].kind != SourceKind::Fm) rows_[w++] = rows_[i];
  }
  count_ = w;
}

void SourceTable::putRow(const SourceRow& r) {
  SourceRow* dst = rowFor(r.kind, r.freq);
  if (dst != nullptr) *dst = r;
}

void SourceTable::noteTime(SourceRow* r, int64_t err_us, bool confirmable) {
  if (r == nullptr) return;
  if (r->heard < 0xFFFF) ++r->heard;
  r->silent = 0;
  if (!confirmable) return;   // spoken, not checked: that is what Yellow means

  const int64_t ms = err_us / 1000;
  r->err_ms = static_cast<int16_t>(ms > 32000 ? 32000 : ms < -32000 ? -32000 : ms);
  r->err_known = true;
  const int64_t mag = iabs64(err_us);
  if (mag <= cfg_.agree_us) {
    if (r->confirmed < 0xFFFF) ++r->confirmed;
    r->wrong = false;
  } else {
    r->wrong = mag > cfg_.wrong_us;
  }
}

void SourceTable::noteConfirmed(SourceRow* r) {
  if (r == nullptr || r->heard == 0) return;
  if (r->confirmed < r->heard) ++r->confirmed;
  r->wrong = false;
  r->silent = 0;
  r->err_known = false;   // confirmed by another's word, not a fresh measurement
}

void SourceTable::noteWrong(SourceRow* r, int64_t err_us) {
  if (r == nullptr) return;
  if (r->heard < 0xFFFF) ++r->heard;
  r->silent = 0;
  const int64_t ms = err_us / 1000;
  r->err_ms = static_cast<int16_t>(ms > 32000 ? 32000 : ms < -32000 ? -32000 : ms);
  r->err_known = true;
  r->wrong = true;
}

SourceRow* SourceTable::findPi(uint16_t pi) {
  if (pi == 0) return nullptr;
  for (std::size_t i = 0; i < count_; ++i) {
    if (rows_[i].kind == SourceKind::Fm && rows_[i].pi == pi) return &rows_[i];
  }
  return nullptr;
}

void SourceTable::noteDwellEnd(SourceRow* r, bool got_time) {
  if (r == nullptr) return;
  if (r->tries < 0xFF) ++r->tries;
  if (got_time) {
    r->silent = 0;
  } else if (r->silent < 0xFF) {
    ++r->silent;
  }
}

Rating SourceTable::rate(const SourceRow& r, const SourceTableConfig& cfg) {
  if (r.wrong) return Rating::Red;
  if (r.silent >= cfg.silent_for_red) return Rating::Red;
  if (r.confirmed > 0) {
    // Confirmed once, but its latest word is sloppy (between agreeing and
    // wrong): still probably good, no longer proven good.
    const int64_t last = static_cast<int64_t>(r.err_ms) * 1000;
    if (r.err_known && iabs64(last) > cfg.agree_us) return Rating::Yellow;
    return Rating::Green;
  }
  if (r.heard > 0) return Rating::Yellow;
  return Rating::Unknown;
}

char SourceTable::letter(Rating r) {
  switch (r) {
    case Rating::Green: return 'G';
    case Rating::Yellow: return 'Y';
    case Rating::Red: return 'R';
    default: return '?';
  }
}

void SourceTable::name(const SourceRow& r, Rating rt, char* buf, std::size_t n) {
  if (buf == nullptr || n == 0) return;
  const char l = letter(rt);
  if (r.kind == SourceKind::Fm) {
    std::snprintf(buf, n, "%c %ld.%ld", l, static_cast<long>(r.freq / 100),
                  static_cast<long>((r.freq / 10) % 10));
  } else if (r.freq % 1000 != 0) {
    std::snprintf(buf, n, "%c WWV %ld.%ld", l, static_cast<long>(r.freq / 1000),
                  static_cast<long>((r.freq % 1000) / 100));
  } else {
    std::snprintf(buf, n, "%c WWV %ld", l, static_cast<long>(r.freq / 1000));
  }
}

std::size_t SourceTable::encode(void* buf, std::size_t cap) const {
  const std::size_t need = 2 + count_ * kRowBytes;
  if (buf == nullptr || cap < need) return 0;
  uint8_t* p = static_cast<uint8_t*>(buf);
  *p++ = kVersion;
  *p++ = static_cast<uint8_t>(count_);
  for (std::size_t i = 0; i < count_; ++i) {
    const SourceRow& r = rows_[i];
    *p++ = static_cast<uint8_t>(r.kind);
    put32(p, r.freq);
    put16(p, r.pi);
    put16(p, r.heard);
    put16(p, r.confirmed);
    *p++ = r.silent;
    *p++ = r.tries;
    *p++ = static_cast<uint8_t>((r.wrong ? 1 : 0) | (r.err_known ? 2 : 0));
    put16(p, static_cast<uint16_t>(r.err_ms));
  }
  return need;
}

bool SourceTable::decode(const void* buf, std::size_t len) {
  if (buf == nullptr || len < 2) return false;
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  if (*p++ != kVersion) return false;
  const std::size_t n = *p++;
  if (n > kMaxRows || len != 2 + n * kRowBytes) return false;

  // Verify everything before touching the table: a half-believed blob would
  // invent a rating, and a false Green is worse than relearning.
  SourceRow tmp[kMaxRows];
  for (std::size_t i = 0; i < n; ++i) {
    SourceRow r;
    const uint8_t kind = *p++;
    if (kind > static_cast<uint8_t>(SourceKind::Wwv)) return false;
    r.kind = static_cast<SourceKind>(kind);
    r.freq = get32(p);
    if (r.freq <= 0) return false;
    r.pi = get16(p);
    r.heard = get16(p);
    r.confirmed = get16(p);
    r.silent = *p++;
    r.tries = *p++;
    const uint8_t flags = *p++;
    if (flags > 3) return false;
    r.wrong = (flags & 1) != 0;
    r.err_known = (flags & 2) != 0;
    r.err_ms = static_cast<int16_t>(get16(p));
    if (r.confirmed > r.heard) return false;
    tmp[i] = r;
  }
  for (std::size_t i = 0; i < n; ++i) rows_[i] = tmp[i];
  count_ = n;
  return true;
}

}  // namespace airtime
