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

 private:
  Preferences prefs_;
  bool open_ = false;
};

}  // namespace airtime_esp32
