#include "sntp.h"

namespace airtime {
namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint32_t get32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void put64(uint8_t* p, uint64_t v) {
  put32(p, static_cast<uint32_t>(v >> 32));
  put32(p + 4, static_cast<uint32_t>(v));
}

uint64_t get64(const uint8_t* p) {
  return (static_cast<uint64_t>(get32(p)) << 32) | static_cast<uint64_t>(get32(p + 4));
}

}  // namespace

uint32_t refIdForSource(Source s) {
  switch (s) {
    case Source::Wwv: return kRefIdWwv;
    case Source::Rds: return kRefIdRds;
    case Source::Manual: return kRefIdManual;
    case Source::None: break;
  }
  return 0;
}

uint64_t unixUsToNtp(int64_t unix_us) {
  int64_t s = unix_us / 1000000;
  int64_t us = unix_us % 1000000;
  if (us < 0) {  // floor toward -inf so the fraction stays positive
    us += 1000000;
    s -= 1;
  }
  const uint32_t secs = static_cast<uint32_t>(s + kNtpUnixDeltaS);
  const uint32_t frac = static_cast<uint32_t>((us << 32) / 1000000);
  return (static_cast<uint64_t>(secs) << 32) | frac;
}

int64_t ntpToUnixUs(uint64_t ntp) {
  const int64_t secs = static_cast<int64_t>(ntp >> 32) - kNtpUnixDeltaS;
  const uint32_t frac = static_cast<uint32_t>(ntp);
  const int64_t us = (static_cast<int64_t>(frac) * 1000000) >> 32;
  return secs * 1000000 + us;
}

uint32_t usToNtpShort(int64_t us) {
  if (us <= 0) return 0;
  const int64_t s = us / 1000000;
  if (s >= 65536) return 0xFFFFFFFFu;
  const int64_t rem = us % 1000000;
  const uint32_t frac = static_cast<uint32_t>((rem << 16) / 1000000);
  return (static_cast<uint32_t>(s) << 16) | frac;
}

bool parseSntpRequest(const uint8_t* buf, std::size_t len, SntpRequest* out) {
  if (buf == nullptr || len < kNtpPacketSize) return false;

  const uint8_t b0 = buf[0];
  const uint8_t version = (b0 >> 3) & 0x07;
  const uint8_t mode = b0 & 0x07;

  if (mode != kNtpModeClient) return false;   // only client requests are served
  if (version < 1 || version > 4) return false;

  if (out != nullptr) {
    out->version = version;
    out->mode = mode;
    out->poll = buf[2];
    out->transmit_ntp = get64(buf + 40);
  }
  return true;
}

void buildSntpResponse(const SntpRequest& req, int64_t recv_utc_us,
                       int64_t xmit_utc_us, const SntpServerState& st,
                       uint8_t* resp) {
  if (resp == nullptr) return;
  for (std::size_t i = 0; i < kNtpPacketSize; ++i) resp[i] = 0;

  const uint8_t li = st.synced ? kNtpLeapNone : kNtpLeapAlarm;
  resp[0] = static_cast<uint8_t>((li << 6) | ((req.version & 0x07) << 3) |
                                 kNtpModeServer);
  resp[1] = st.synced ? kNtpStratumPrimary : kNtpStratumUnsync;
  resp[2] = req.poll;
  resp[3] = static_cast<uint8_t>(st.precision);

  put32(resp + 4, 0);  // root delay: zero, we are a primary reference
  put32(resp + 8, usToNtpShort(st.uncertainty_us));  // honest error bars
  put32(resp + 12, refIdForSource(st.source));

  put64(resp + 16, st.last_sync_utc_us != 0 ? unixUsToNtp(st.last_sync_utc_us) : 0);
  put64(resp + 24, req.transmit_ntp);            // origin = client's transmit
  put64(resp + 32, unixUsToNtp(recv_utc_us));    // receive
  put64(resp + 40, unixUsToNtp(xmit_utc_us));    // transmit
}

bool handleSntpRequest(const uint8_t* req_buf, std::size_t req_len,
                       int64_t recv_utc_us, int64_t xmit_utc_us,
                       const SntpServerState& st, uint8_t* resp) {
  SntpRequest req;
  if (!parseSntpRequest(req_buf, req_len, &req)) return false;
  buildSntpResponse(req, recv_utc_us, xmit_utc_us, st, resp);
  return true;
}

void ClientCounter::touch(uint32_t client_id, int64_t mono_us) {
  for (std::size_t i = 0; i < count_; ++i) {
    if (ids_[i] == client_id) {
      last_seen_[i] = mono_us;
      return;
    }
  }
  if (count_ < kMaxClients) {
    ids_[count_] = client_id;
    last_seen_[count_] = mono_us;
    ++count_;
    return;
  }
  // Table full: evict the least recently seen entry.
  std::size_t oldest = 0;
  for (std::size_t i = 1; i < count_; ++i) {
    if (last_seen_[i] < last_seen_[oldest]) oldest = i;
  }
  ids_[oldest] = client_id;
  last_seen_[oldest] = mono_us;
}

int ClientCounter::countActive(int64_t mono_us, int64_t window_us) const {
  int n = 0;
  for (std::size_t i = 0; i < count_; ++i) {
    if (mono_us - last_seen_[i] <= window_us) ++n;
  }
  return n;
}

void ClientCounter::clear() { count_ = 0; }

}  // namespace airtime
