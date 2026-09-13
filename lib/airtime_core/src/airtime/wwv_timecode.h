#pragma once
//
// WWV/WWVH 100 Hz time code — the thing that makes this device work with no FM.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// PLAN.md §3 settled v1 as "no WWV date/timecode decode (phase only)", and
// that decision is the root of the device's worst structural weakness. The
// 1000 Hz minute marker gives phase but not identity: it says where the second
// boundary is, not WHICH minute. So WWV cannot start a clock, only refine one —
// and every fix has to come from RDS first. A time reference built for the
// hurricane case then depends entirely on a broadcast FM station being
// receivable, which at the owner's QTH means a marginal signal 50 miles away.
//
// The 100 Hz subcarrier carries the answer: minute, hour, day-of-year, year.
// Absolute time and date, from HF alone.
//
// ── The code ────────────────────────────────────────────────────────────────
//
// A modified IRIG-H code on a 100 Hz subcarrier, one bit per second, 60 bits
// to a frame, one frame per minute. Each second opens with a pulse of 100 Hz
// whose LENGTH is the symbol:
//
//     170 ms  binary zero
//     470 ms  binary one
//     770 ms  position marker
//
// Position markers fall on seconds 9, 19, 29, 39, 49 and 59, which is what
// lets a receiver find the frame without knowing the time already.
//
// ── Why the bit map is a table ──────────────────────────────────────────────
//
// The field positions below are assembled from NIST's published format and a
// second source, but they are NOT verified against this station off the air,
// and a wrong weight here would produce a clock that is confidently, silently
// wrong — the exact failure this project has been bitten by three times.
//
// So: the map is DATA, correctable in one line without touching logic, and
// rawFrame() hands out the undecoded 60 symbols so a single capture from the
// field can confirm or correct it empirically. Until then the corroboration
// rule below is what stands between a wrong table and a wrong clock.
//
// ── Nothing is trusted until it repeats ─────────────────────────────────────
//
// A frame is never reported on its own evidence. Two frames must decode and
// read an EXACT WHOLE NUMBER of minutes apart — epoch arithmetic, computed
// through the calendar, not field-by-field comparison. A mis-synced frame, a
// wrong bit weight, or noise that happens to pass the width gates will not
// survive that, because it would have to fail twice in exactly the way that
// advances a clock. Up to three minutes may separate the pair, because on a
// fading HF path the frame BETWEEN two clean ones is often the casualty — and
// a lost middle frame says nothing against the two that agree across it.
//
// ── Holes ───────────────────────────────────────────────────────────────────
//
// A frame with an unreadable second in it is discarded, never guessed at. But
// discarding the frame is not the same as abandoning the alignment: if the
// seven structural markers (second 0 and the six position markers) all read
// correctly, the frame boundary is certainly right and only the data was lost
// — so the decoder keeps its place and lets the next minute try, instead of
// spending up to a minute re-hunting for an alignment it never lost. Only a
// broken marker skeleton forces a re-hunt.

#include <cstddef>
#include <cstdint>

namespace airtime {

// What one second's pulse turned out to be.
enum class TcSymbol : uint8_t { Zero, One, Marker, Invalid };

struct WwvTimecodeConfig {
  // Half-width of the acceptance window around each nominal pulse length.
  // Generous, because this is an envelope measured through an AM receiver on a
  // fading HF path, not a wire. The three nominal widths are 300 ms apart, so
  // even ±100 ms cannot make one symbol read as another.
  int64_t tolerance_ms = 90;
  int64_t zero_ms = 170;
  int64_t one_ms = 470;
  int64_t marker_ms = 770;
};

// One decoded minute.
struct WwvTime {
  int minute = 0;      // 0..59
  int hour = 0;        // 0..23  UTC
  int day_of_year = 0; // 1..366
  int year = 0;        // full year, century inferred (see decode())
  bool dst_now = false;
  bool dst_soon = false;
  bool leap_warning = false;
  int dut1_ms = 0;     // signed, -800..+800
  bool valid = false;
};

// Classify a single pulse length. Exposed for testing and for the status page.
TcSymbol classifyPulse(int64_t pulse_ms, const WwvTimecodeConfig& cfg);

// Turn 60 symbols into a time. Returns valid=false on any range violation or
// missing position marker. `century_hint_year` resolves the code's two-digit
// year — WWV sends only the last two digits, so SOMETHING has to supply the
// century, and a compile-time build year is the honest choice: it cannot be
// wrong by less than 100 years and it needs no other source.
WwvTime decodeFrame(const TcSymbol* sym, std::size_t n, int century_hint_year);

// The inverse: the 60 symbols WWV would transmit for a given minute. Built
// from the SAME weight tables as decodeFrame, so it proves round-trip
// consistency and nothing more — it is for the simulator and the fakes, and
// it is NOT evidence that the table matches Fort Collins. (The independent
// transcription lives in test_wwv_timecode.cpp; the air capture via
// rawFrame() is the only verification that counts.)
void encodeFrame(const WwvTime& t, TcSymbol out[60]);

// Epoch seconds -> the WwvTime WWV would be transmitting during that minute.
// Seconds are floored away; DUT1 and the flags are zeroed (the sim's world
// has no DUT1). Companion to wwvTimeToEpochS below, for the same users.
WwvTime wwvTimeFromEpochS(int64_t epoch_s);

// Feed it one second's pulse length at a time; it finds the frame, decodes it,
// and refuses to believe itself until a second frame agrees.
class WwvTimecodeDecoder {
 public:
  explicit WwvTimecodeDecoder(const WwvTimecodeConfig& cfg = {},
                              int century_hint_year = 2026)
      : cfg_(cfg), century_hint_(century_hint_year) {}

  // `pulse_ms` is the measured length of the 100 Hz burst that opened this
  // second, and `second_start_us` is the monotonic timestamp of its leading
  // edge — the instant the reported time refers to.
  //
  // Returns true exactly once per confirmed frame. The time reported is that
  // of the frame's OWN minute, at the second-0 boundary, so the caller applies
  // it against second_start_us of the frame's first second (see epochAt).
  bool onSecond(int64_t pulse_ms, int64_t second_start_us);

  const WwvTime& time() const { return confirmed_; }
  // Monotonic timestamp of second 0 of the confirmed frame.
  int64_t frameStartUs() const { return confirmed_start_us_; }

  // The raw symbols of the most recent COMPLETE frame — a snapshot taken the
  // moment second 59 lands, so a caller polling on its own schedule never
  // reads a frame half-overwritten by the next minute. This is how a wrong
  // bit map gets found: one photograph of 60 symbols beside a known UTC and
  // the table can be corrected without guesswork.
  const TcSymbol* rawFrame() const { return last_frame_; }
  std::size_t rawFrameLen() const { return kFrameBits; }

  uint32_t framesSeen() const { return frames_seen_; }
  // Frames that decoded to a plausible time (pre-corroboration). This moving
  // is the band-is-delivering signal: structure this specific does not come
  // from noise, even when the time it carries is still awaiting its partner.
  uint32_t framesDecoded() const { return frames_decoded_; }
  uint32_t framesConfirmed() const { return frames_confirmed_; }
  bool haveFrame() const { return confirmed_.valid; }

  void reset();

  static constexpr std::size_t kFrameBits = 60;

 private:
  WwvTimecodeConfig cfg_;
  int century_hint_;

  TcSymbol frame_[kFrameBits] = {};
  TcSymbol last_frame_[kFrameBits] = {};  // snapshot for rawFrame()
  std::size_t fill_ = 0;          // how many seconds of this frame we hold
  bool framed_ = false;           // have we found second 0 yet
  bool last_was_marker_ = false;  // for the 59/0 adjacent-marker hunt
  int64_t frame_start_us_ = 0;

  WwvTime pending_;               // last decoded frame, awaiting corroboration
  int64_t pending_start_us_ = 0;

  WwvTime confirmed_;
  int64_t confirmed_start_us_ = 0;

  uint32_t frames_seen_ = 0;
  uint32_t frames_decoded_ = 0;
  uint32_t frames_confirmed_ = 0;
};

// Seconds since the Unix epoch for a decoded time. Day-of-year is converted
// through the civil calendar, so leap years land correctly.
int64_t wwvTimeToEpochS(const WwvTime& t);

}  // namespace airtime
