#pragma once
//
// What the radio has LEARNED, in a form that survives a power cycle.
//
// Two facts are earned slowly and thrown away instantly at power-off:
//
//   * how late each station runs — hours of WWV windows to measure, and
//     without it a badly-biased station is rejected on sight (station_bias.h);
//   * which HF band actually propagates here — every cold boot otherwise
//     re-hunts 5 MHz before rediscovering that 15 MHz is the one that works.
//
// Both are properties of the world around this radio, not of this power-on, so
// they belong in NVS beside the drift figure.
//
// ── Format rules, and why they are strict ───────────────────────────────────
//
// This data comes back from flash that may have been written by an older
// build, a half-finished write, or a different device. A decoder that trusts
// it can silently poison a working clock with a fabricated station bias, which
// is a far worse failure than simply relearning. So every record is
// fixed-width, the payload is length-checked against its own count, and a
// version byte that does not match causes the blob to be ignored outright.
// Decode is all-or-nothing: a blob that does not add up leaves the target
// untouched rather than half-applied.

#include <cstddef>
#include <cstdint>

#include "scheduler.h"
#include "station_bias.h"

namespace airtime {

// Keys used with ITimeStore::loadBlob / saveBlob. Short: NVS keys are capped
// at 15 characters.
constexpr const char* kBlobStationBias = "sta";
constexpr const char* kBlobBandStats = "band";
constexpr const char* kBlobStations = "fm";

// Worst-case encoded sizes, for caller-side buffers.
constexpr std::size_t kStationBiasBlobMax = 2 + StationBiasTable::kMaxStations * 8;
constexpr std::size_t kBandStatsBlobMax = 2 + Scheduler::kMaxBands * 10;
constexpr std::size_t kStationsBlobMax = 2 + 8 * 4;

// Returns bytes written, or 0 if `cap` is too small.
std::size_t encodeStationBias(const StationBiasTable& t, void* buf, std::size_t cap);
// Returns false and changes nothing if the blob is absent, truncated, corrupt
// or of an unknown version.
bool decodeStationBias(const void* buf, std::size_t len, StationBiasTable* out);

std::size_t encodeBandStats(const Scheduler& s, void* buf, std::size_t cap);

// The surveyed FM station list. Returns bytes written / stations read.
std::size_t encodeStations(const int32_t* khz, std::size_t n, void* buf,
                           std::size_t cap);
std::size_t decodeStations(const void* buf, std::size_t len, int32_t* khz_out,
                           std::size_t max);
bool decodeBandStats(const void* buf, std::size_t len, Scheduler* out);

}  // namespace airtime
