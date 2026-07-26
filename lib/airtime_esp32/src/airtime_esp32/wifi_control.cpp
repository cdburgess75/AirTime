#include "wifi_control.h"

#include <cstring>

#include <WiFi.h>
#include <WiFiUdp.h>

#include <airtime/app.h>
#include <airtime/sntp.h>

namespace airtime_esp32 {

namespace {
// One socket for the device. Kept out of the header so <WiFiUdp.h> does not
// leak into every includer.
WiFiUDP g_udp;
}  // namespace

void Esp32WiFiControl::bringUp() {
  if (up_) return;

  // Called every loop pass while the directive wants WiFi and it is not up, so
  // a failed attempt retries itself — but at a civilised rate, not per-pass.
  const uint32_t now = millis();
  if (attempts_ > 0 && (now - last_attempt_ms_) < 500) return;
  last_attempt_ms_ = now;
  ++attempts_;

  WiFi.persistent(false);  // never write AP config to the chip's own NVS
  bool ok = WiFi.mode(WIFI_AP);

  const char* pass = cfg_.password;
  if (pass != nullptr && std::strlen(pass) < 8) pass = nullptr;
  ok = WiFi.softAP(cfg_.ssid, pass, cfg_.channel, /*ssid_hidden=*/0,
                   cfg_.max_clients) && ok;

  // These calls DO fail in this design, and claiming up_ regardless made one
  // transient failure into a silent outage: the AP only retried at the next
  // listen-window cycle, minutes to an hour later, while the app printed a
  // healthy status line throughout (observed in the field — the operator's
  // laptop saw no SSID for the better part of an hour). The likeliest trigger
  // is the one the architecture itself warns about: bringUp runs moments after
  // the WWV sampler is told to stop, and ADC2 contends with the WiFi radio in
  // silicon. The sampler now parks before stop() returns; this check is the
  // seatbelt for every other way esp_wifi can say no.
  if (!ok) {
    ++failures_;
    WiFi.mode(WIFI_MODE_NULL);  // clean slate for the retry
    return;                     // up_ stays false; applyDirective retries us
  }

  WiFi.setTxPower(static_cast<wifi_power_t>(cfg_.tx_power));
  g_udp.begin(cfg_.ntp_port);
  up_ = true;
}

void Esp32WiFiControl::tearDown() {
  if (!up_) return;
  g_udp.stop();
  WiFi.softAPdisconnect(true);
  // Radio fully off. "Down" must mean down or ADC2 stays unreadable — see the
  // header. WIFI_MODE_NULL is the same call stock ats-mini's netStop() makes,
  // and it is the state Milestone 0 verified a clean ADC against.
  WiFi.mode(WIFI_MODE_NULL);
  up_ = false;
}

void Esp32WiFiControl::service(airtime::AirTimeApp& app) {
  if (!up_) return;

  // Liveness: if esp_wifi died underneath us (the AP mode bit is gone), admit
  // it, so applyDirective re-raises the AP instead of serving into the void.
  const wifi_mode_t m = WiFi.getMode();
  if (m != WIFI_MODE_AP && m != WIFI_MODE_APSTA) {
    ++failures_;
    g_udp.stop();
    up_ = false;
    return;
  }

  // Bounded per pass: real traffic is one client asking every few seconds.
  for (int i = 0; i < 4; ++i) {
    const int n = g_udp.parsePacket();
    if (n <= 0) return;

    uint8_t req[airtime::kNtpPacketSize];
    uint8_t resp[airtime::kNtpPacketSize];
    // Overlength datagrams are fine to truncate: the next parsePacket()
    // discards whatever is left of this one.
    const int got = g_udp.read(req, sizeof(req));

    const uint32_t client = static_cast<uint32_t>(g_udp.remoteIP());
    if (got < static_cast<int>(sizeof(req))) continue;  // runt: not NTP
    if (!app.handleNtpRequest(req, sizeof(req), client, resp)) continue;

    g_udp.beginPacket(g_udp.remoteIP(), g_udp.remotePort());
    g_udp.write(resp, sizeof(resp));
    g_udp.endPacket();
    ++served_;
  }
}

int Esp32WiFiControl::stationCount() const {
  return up_ ? WiFi.softAPgetStationNum() : 0;
}

}  // namespace airtime_esp32
