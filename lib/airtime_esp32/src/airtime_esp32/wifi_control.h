#pragma once
//
// IWiFiControl as a SoftAP plus the NTP/UDP socket — PLAN.md §4, §5.
//
// AirTime serves time to a laptop that has no working time source of its own,
// so the device is the network: bringUp() raises an access point (no existing
// infrastructure required) and opens UDP/123; the laptop joins the AP and asks
// 192.168.4.1 for NTP.
//
// ── The one rule that matters ───────────────────────────────────────────────
//
// tearDown() must actually power the radio off — WIFI_MODE_NULL, not just
// disconnect. ADC2 (the WWV audio tap on GPIO11) is unreadable while the WiFi
// radio is active; this is ESP32 silicon, not firmware (PLAN.md §2), and it
// was confirmed on the owner's unit in Milestone 0: WiFi up means a clipped,
// wandering ADC. The Scheduler sequences tearDown() before listening starts,
// and this adapter's job is to make "down" mean down.
//
// ── Serving semantics ───────────────────────────────────────────────────────
//
// service() is polled from the main loop, so a response's timestamps are taken
// a few milliseconds after the datagram actually arrived (worst case ~100 ms
// when the host firmware blocks on UI work). That skew lands inside the
// uncertainty the device is already reporting via root dispersion, and NTP
// clients smooth over per-packet jitter by design. A dedicated socket task is
// not worth its complexity at v1's accuracy target.

#include <cstdint>

#include <airtime/hal.h>

namespace airtime {
class AirTimeApp;
}

namespace airtime_esp32 {

struct WiFiControlConfig {
  const char* ssid = "AirTime";
  // WPA2 needs >= 8 characters; empty or short means an open AP. Open is the
  // field-friendly default: the only thing on offer is 48-byte time datagrams.
  const char* password = nullptr;
  uint8_t channel = 6;
  uint8_t max_clients = 4;
  // Stock ats-mini pins TX power to 17 dBm on this board (see Network.cpp);
  // mirror that unless told otherwise. Value is a wifi_power_t.
  int tx_power = 34;  // WIFI_POWER_17dBm
  uint16_t ntp_port = 123;
};

class Esp32WiFiControl : public airtime::IWiFiControl {
 public:
  Esp32WiFiControl() = default;

  void begin(const WiFiControlConfig& cfg) { cfg_ = cfg; }

  // --- IWiFiControl ----------------------------------------------------------
  void bringUp() override;
  void tearDown() override;
  bool isUp() const override { return up_; }

  // Answer any NTP datagrams that have arrived. Call every loop while up.
  void service(airtime::AirTimeApp& app);

  // --- Diagnostics -----------------------------------------------------------
  int stationCount() const;       // clients currently joined to the AP
  uint32_t requestsServed() const { return served_; }
  // bringUp attempts that esp_wifi refused, plus liveness checks that found the
  // AP dead. Nonzero is worth a look; climbing means the radio cannot start —
  // and the status line prints it, so a silent outage can no longer happen.
  uint32_t upFailures() const { return failures_; }

 private:
  WiFiControlConfig cfg_;
  bool up_ = false;
  uint32_t served_ = 0;
  uint32_t attempts_ = 0;
  uint32_t failures_ = 0;
  uint32_t last_attempt_ms_ = 0;
};

}  // namespace airtime_esp32
