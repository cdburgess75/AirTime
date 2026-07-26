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

  WiFi.persistent(false);  // never write AP config to the chip's own NVS
  WiFi.mode(WIFI_AP);

  const char* pass = cfg_.password;
  if (pass != nullptr && std::strlen(pass) < 8) pass = nullptr;
  WiFi.softAP(cfg_.ssid, pass, cfg_.channel, /*ssid_hidden=*/0, cfg_.max_clients);
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
