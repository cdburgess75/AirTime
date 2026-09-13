#pragma once
//
// ITimeStore over NVS (Preferences) — PLAN.md §4 "learn the crystal".
//
// Two values survive power loss: the learned drift rate, so a warm boot starts
// with a characterised crystal instead of the ±20 ppm datasheet guess, and the
// last known UTC, so the device can come up saying "roughly now, UNSYNCED"
// rather than knowing nothing (the arbiter restores it with honest, hour-scale
// uncertainty — see AirTimeApp::begin).
//
// Flash wear is the app's problem, already solved: persist() writes at most
// once an hour (drift_save_interval_us), and NVS itself no-ops writes whose
// value did not change. This adapter stays dumb on purpose.

#include <cstdint>

#include <Preferences.h>

#include <airtime/hal.h>

namespace airtime_esp32 {

class NvsTimeStore : public airtime::ITimeStore {
 public:
  NvsTimeStore() = default;
  ~NvsTimeStore() override { end(); }

  // Opens the "airtime" namespace read-write. Returns false if NVS is broken;
  // the app treats a null/failed store as "no persistence" and runs fine.
  bool begin() {
    if (open_) return true;
    open_ = prefs_.begin("airtime", /*readOnly=*/false);
    return open_;
  }

  void end() {
    if (!open_) return;
    prefs_.end();
    open_ = false;
  }

  // Erase everything AirTime has ever learned or been told — drift, last
  // time, station biases, band stats, surveyed stations, menu settings. The
  // recovery hammer for a radio whose persisted brain is suspected of being
  // the problem: learning that survives power-off also lets a bad lesson
  // survive power-off, and this is the honest way out. The caller reboots
  // afterwards; a fresh boot relearns everything from the air.
  bool wipeAll() {
    if (!open_) return false;
    return prefs_.clear();
  }

  // --- ITimeStore ------------------------------------------------------------
  bool loadDriftPpm(double* ppm) override {
    if (!open_ || !prefs_.isKey("drift")) return false;
    *ppm = prefs_.getDouble("drift", 0.0);
    return true;
  }

  void saveDriftPpm(double ppm) override {
    if (open_) prefs_.putDouble("drift", ppm);
  }

  bool loadLastUtc(int64_t* utc_us) override {
    if (!open_ || !prefs_.isKey("utc")) return false;
    *utc_us = prefs_.getLong64("utc", 0);
    return true;
  }

  void saveLastUtc(int64_t utc_us) override {
    if (open_) prefs_.putLong64("utc", utc_us);
  }

  // Learned state (station biases, band propagation) as opaque blobs. The
  // core owns the format and validates everything it reads back — see
  // learned_state.h — so this stays a dumb pipe.
  bool loadBlob(const char* key, void* buf, std::size_t cap,
                std::size_t* out_len) override {
    if (!open_ || !prefs_.isKey(key)) return false;
    const std::size_t n = prefs_.getBytes(key, buf, cap);
    if (n == 0) return false;   // absent, or too big for the caller's buffer
    if (out_len != nullptr) *out_len = n;
    return true;
  }

  void saveBlob(const char* key, const void* buf, std::size_t len) override {
    if (open_) prefs_.putBytes(key, buf, len);
  }

 private:
  Preferences prefs_;
  bool open_ = false;
};

}  // namespace airtime_esp32
