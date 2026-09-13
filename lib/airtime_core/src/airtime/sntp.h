#pragma once
//
// SNTP server packet layer — PLAN.md Milestone 2 ("SoftAP + NTP server (SNTP
// responder is sufficient), client counting, unsynchronized flagging").
//
// Pure logic: bytes in, bytes out. The device adapter owns the UDP socket on
// port 123 and simply hands us the request buffer plus two timestamps (when the
// request arrived, when the reply is going out). Everything about NTP framing,
// epoch conversion, and honest status reporting lives here and is host-tested.
//
// Honesty (PLAN.md §4 rule 5) is expressed in the wire protocol itself:
//   * synced   -> leap indicator 0, stratum 1 (primary reference), reference
//                 identifier naming the actual source ("WWV" / "RDS").
//   * unsynced -> leap indicator 3 (alarm) and stratum 16 (unsynchronized),
//                 which every sane NTP client treats as "do not use".
//   * the arbiter's uncertainty is published as root dispersion, so a client
//     can see our error bars rather than having to trust us.
//
// NTP v4 header (RFC 5905 §7.3), 48 bytes, big-endian on the wire:
//   0      LI(2) VN(3) Mode(3)
//   1      Stratum
//   2      Poll
//   3      Precision (signed)
//   4-7    Root Delay        (16.16 fixed point seconds)
//   8-11   Root Dispersion   (16.16 fixed point seconds)
//   12-15  Reference ID
//   16-23  Reference Timestamp (64-bit NTP)
//   24-31  Origin Timestamp
//   32-39  Receive Timestamp
//   40-47  Transmit Timestamp

#include <cstddef>
#include <cstdint>

#include "types.h"

namespace airtime {

constexpr std::size_t kNtpPacketSize = 48;

// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr int64_t kNtpUnixDeltaS = 2208988800LL;

constexpr uint8_t kNtpModeClient = 3;
constexpr uint8_t kNtpModeServer = 4;
constexpr uint8_t kNtpLeapNone = 0;
constexpr uint8_t kNtpLeapAlarm = 3;      // unsynchronized
constexpr uint8_t kNtpStratumPrimary = 1; // radio clock
constexpr uint8_t kNtpStratumUnsync = 16;

// Four-character NTP reference identifier, as it sits on the wire.
constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(d));
}
// "WWV" is a registered RFC 5905 identifier. "RDS" is not registered (no code
// exists for broadcast RDS clock-time); we use it as a descriptive local code.
constexpr uint32_t kRefIdWwv = fourcc('W', 'W', 'V', '\0');
constexpr uint32_t kRefIdRds = fourcc('R', 'D', 'S', '\0');
constexpr uint32_t kRefIdManual = fourcc('M', 'A', 'N', '\0');

uint32_t refIdForSource(Source s);

// --- Timestamp conversion (era 0; valid until 2036-02-07) -------------------
uint64_t unixUsToNtp(int64_t unix_us);
int64_t ntpToUnixUs(uint64_t ntp);

// Convert a duration in µs to NTP "short format" (16.16 fixed point seconds),
// saturating at ~65536 s.
uint32_t usToNtpShort(int64_t us);

// --- Request parsing --------------------------------------------------------
struct SntpRequest {
  uint8_t version = 4;
  uint8_t mode = 0;
  uint8_t poll = 0;
  uint64_t transmit_ntp = 0;  // becomes the response's origin timestamp
};

// Returns false unless the buffer is a well-formed client-mode request.
bool parseSntpRequest(const uint8_t* buf, std::size_t len, SntpRequest* out);

// --- Response building ------------------------------------------------------
struct SntpServerState {
  bool synced = false;
  int64_t uncertainty_us = 0;    // published as root dispersion
  int64_t last_sync_utc_us = 0;  // published as the reference timestamp
  Source source = Source::None;  // published as the reference identifier
  int8_t precision = -20;        // log2(seconds); -20 ~= 1 µs (esp_timer)
};

// Build a 48-byte server response. `resp` must have room for kNtpPacketSize.
// recv_utc_us  : our UTC estimate when the request arrived
// xmit_utc_us  : our UTC estimate as the response goes out
void buildSntpResponse(const SntpRequest& req, int64_t recv_utc_us,
                       int64_t xmit_utc_us, const SntpServerState& st,
                       uint8_t* resp);

// Convenience: parse + build in one call. Returns false (writing nothing) if the
// request is not a valid client request.
bool handleSntpRequest(const uint8_t* req_buf, std::size_t req_len,
                       int64_t recv_utc_us, int64_t xmit_utc_us,
                       const SntpServerState& st, uint8_t* resp);

// --- Client counting (Milestone 2: "NTP: 2 clients" on the display) ---------
class ClientCounter {
 public:
  static constexpr std::size_t kMaxClients = 16;

  // Record activity from a client (IPv4 address, or any stable id).
  void touch(uint32_t client_id, int64_t mono_us);

  // Distinct clients seen within `window_us` of `mono_us`.
  int countActive(int64_t mono_us, int64_t window_us) const;

  void clear();
  std::size_t tracked() const { return count_; }

 private:
  uint32_t ids_[kMaxClients] = {};
  int64_t last_seen_[kMaxClients] = {};
  std::size_t count_ = 0;
};

}  // namespace airtime
