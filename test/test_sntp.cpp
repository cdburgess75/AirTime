#include <cstdint>
#include <cstring>

#include "airtime/sntp.h"
#include "test_framework.h"

using namespace airtime;

namespace {

// A minimal well-formed client request.
void makeRequest(uint8_t* buf, uint8_t version, uint8_t mode, uint64_t xmit) {
  std::memset(buf, 0, kNtpPacketSize);
  buf[0] = static_cast<uint8_t>((0 << 6) | ((version & 0x07) << 3) | (mode & 0x07));
  buf[2] = 6;  // poll
  for (int i = 0; i < 8; ++i)
    buf[40 + i] = static_cast<uint8_t>(xmit >> (56 - 8 * i));
}

uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t rd64(const uint8_t* p) {
  return (static_cast<uint64_t>(rd32(p)) << 32) | rd32(p + 4);
}

constexpr int64_t kUtc = 1784000000LL * 1000000LL;  // some 2026 UTC, µs

}  // namespace

// The NTP epoch anchor: Unix 0 is NTP seconds 2208988800.
AT_TEST(sntp_epoch_anchor) {
  const uint64_t t = unixUsToNtp(0);
  AT_CHECK_EQ(static_cast<int64_t>(t >> 32), 2208988800LL);
  AT_CHECK_EQ(static_cast<uint32_t>(t), 0u);
}

// Timestamp conversion round-trips to µs accuracy.
AT_TEST(sntp_timestamp_roundtrip) {
  const int64_t vals[] = {0, 1000000, kUtc, kUtc + 123456, 946684800LL * 1000000};
  for (int64_t v : vals) {
    const int64_t back = ntpToUnixUs(unixUsToNtp(v));
    AT_CHECK(back >= v - 1 && back <= v + 1);
  }
}

// Half a second is 0x8000_0000 in the NTP fraction field.
AT_TEST(sntp_fraction_half_second) {
  const uint64_t t = unixUsToNtp(500000);
  AT_CHECK_EQ(static_cast<int64_t>(static_cast<uint32_t>(t)), 0x80000000LL);
}

// Short format: 1.5 s -> 0x0001_8000.
AT_TEST(sntp_short_format) {
  AT_CHECK_EQ(static_cast<int64_t>(usToNtpShort(1500000)), 0x00018000LL);
  AT_CHECK_EQ(static_cast<int64_t>(usToNtpShort(0)), 0LL);
  AT_CHECK_EQ(static_cast<int64_t>(usToNtpShort(-5)), 0LL);
}

// Only client-mode packets of a sane version are accepted.
AT_TEST(sntp_rejects_non_client) {
  uint8_t buf[kNtpPacketSize];
  SntpRequest req;

  makeRequest(buf, 4, kNtpModeServer, 0);  // server mode
  AT_CHECK(!parseSntpRequest(buf, sizeof(buf), &req));

  makeRequest(buf, 7, kNtpModeClient, 0);  // bogus version
  AT_CHECK(!parseSntpRequest(buf, sizeof(buf), &req));

  makeRequest(buf, 4, kNtpModeClient, 0);  // short buffer
  AT_CHECK(!parseSntpRequest(buf, 20, &req));

  AT_CHECK(!parseSntpRequest(nullptr, kNtpPacketSize, &req));

  makeRequest(buf, 4, kNtpModeClient, 0);  // the good case
  AT_CHECK(parseSntpRequest(buf, sizeof(buf), &req));
  AT_CHECK_EQ(req.version, 4);
  AT_CHECK_EQ(req.poll, 6);
}

// A synced server answers stratum 1, leap 0, with the source's reference id and
// the client's transmit timestamp echoed as origin.
AT_TEST(sntp_synced_response) {
  uint8_t buf[kNtpPacketSize], resp[kNtpPacketSize];
  const uint64_t client_xmit = 0x1122334455667788ULL;
  makeRequest(buf, 4, kNtpModeClient, client_xmit);

  SntpServerState st;
  st.synced = true;
  st.uncertainty_us = 60000;  // ±60 ms
  st.last_sync_utc_us = kUtc - 3600LL * 1000000;
  st.source = Source::Wwv;

  AT_CHECK(handleSntpRequest(buf, sizeof(buf), kUtc, kUtc + 500, st, resp));

  AT_CHECK_EQ((resp[0] >> 6) & 0x03, kNtpLeapNone);  // no alarm
  AT_CHECK_EQ((resp[0] >> 3) & 0x07, 4);             // version echoed
  AT_CHECK_EQ(resp[0] & 0x07, kNtpModeServer);
  AT_CHECK_EQ(resp[1], kNtpStratumPrimary);
  AT_CHECK_EQ(static_cast<int8_t>(resp[3]), -20);

  AT_CHECK_EQ(static_cast<int64_t>(rd32(resp + 4)), 0LL);  // root delay 0
  AT_CHECK_EQ(static_cast<int64_t>(rd32(resp + 8)),
              static_cast<int64_t>(usToNtpShort(60000)));  // dispersion = ±
  AT_CHECK_EQ(static_cast<int64_t>(rd32(resp + 12)),
              static_cast<int64_t>(kRefIdWwv));

  AT_CHECK_EQ(rd64(resp + 24), client_xmit);                 // origin
  AT_CHECK_EQ(rd64(resp + 32), unixUsToNtp(kUtc));           // receive
  AT_CHECK_EQ(rd64(resp + 40), unixUsToNtp(kUtc + 500));     // transmit
  AT_CHECK_EQ(rd64(resp + 16), unixUsToNtp(st.last_sync_utc_us));
}

// An unsynced server flags itself so clients refuse the time (PLAN.md §4 r5).
AT_TEST(sntp_unsynced_is_flagged) {
  uint8_t buf[kNtpPacketSize], resp[kNtpPacketSize];
  makeRequest(buf, 4, kNtpModeClient, 42);

  SntpServerState st;
  st.synced = false;
  st.uncertainty_us = 5000000;
  st.source = Source::None;

  AT_CHECK(handleSntpRequest(buf, sizeof(buf), kUtc, kUtc, st, resp));
  AT_CHECK_EQ((resp[0] >> 6) & 0x03, kNtpLeapAlarm);
  AT_CHECK_EQ(resp[1], kNtpStratumUnsync);
  AT_CHECK_EQ(static_cast<int64_t>(rd32(resp + 12)), 0LL);  // no source
}

// The RDS reference id is used when RDS is the disciplining source.
AT_TEST(sntp_refid_rds) {
  uint8_t buf[kNtpPacketSize], resp[kNtpPacketSize];
  makeRequest(buf, 3, kNtpModeClient, 1);
  SntpServerState st;
  st.synced = true;
  st.source = Source::Rds;
  AT_CHECK(handleSntpRequest(buf, sizeof(buf), kUtc, kUtc, st, resp));
  AT_CHECK_EQ(static_cast<int64_t>(rd32(resp + 12)), static_cast<int64_t>(kRefIdRds));
  AT_CHECK_EQ((resp[0] >> 3) & 0x07, 3);  // v3 client gets a v3 answer
}

// Client counting: distinct clients, dedup, and window expiry.
AT_TEST(sntp_client_counter) {
  ClientCounter c;
  const int64_t s = 1000000;
  c.touch(0xC0A80402, 0);
  c.touch(0xC0A80403, 0);
  c.touch(0xC0A80402, 0);  // repeat of the first
  AT_CHECK_EQ(c.countActive(0, 60 * s), 2);
  AT_CHECK_EQ(c.tracked(), 2u);

  // One client keeps talking, the other goes quiet and ages out.
  c.touch(0xC0A80402, 120 * s);
  AT_CHECK_EQ(c.countActive(120 * s, 60 * s), 1);
  AT_CHECK_EQ(c.countActive(120 * s, 300 * s), 2);

  c.clear();
  AT_CHECK_EQ(c.countActive(120 * s, 300 * s), 0);
}

// The table is bounded: beyond capacity the least-recently-seen entry is evicted.
AT_TEST(sntp_client_counter_bounded) {
  ClientCounter c;
  for (uint32_t i = 0; i < ClientCounter::kMaxClients + 8; ++i) {
    c.touch(0x0A000000u + i, static_cast<int64_t>(i) * 1000);
  }
  AT_CHECK_EQ(c.tracked(), ClientCounter::kMaxClients);
  AT_CHECK(c.countActive(100000000, 200000000) <=
           static_cast<int>(ClientCounter::kMaxClients));
}
