//
// The status page — http://192.168.4.1/ on AirTime's own access point.
//
// This exists because everything worth knowing about why the clock believes
// what it believes has, until now, only ever appeared on a USB serial line.
// Every session of field debugging in this project has been someone squinting
// at `tail -f airtime.log` on a laptop tethered to the radio — while the radio
// was, at that very moment, running an access point with the laptop already
// joined to it. The data was one HTTP GET away the whole time.
//
// ── Why a synchronous server ────────────────────────────────────────────────
//
// ESPAsyncWebServer is vendored here (stock ats-mini uses it) and was the
// obvious choice, but it runs handlers on the AsyncTCP task. Reading AirTimeApp
// from a second thread while the main loop is mutating it is a data race on the
// clock this device exists to keep, and no page is worth that. WebServer.h is
// part of the ESP32 core, needs no extra library, and hands us the request on
// the caller's own stack.
//
// ── Why blocking here is affordable, and only here ──────────────────────────
//
// WebServer::handleClient() can sit waiting on a client that opened a socket
// and then said nothing. That is exactly the failure that made status prints
// non-blocking (Serial.setTxTimeoutMs(0)) after a stalled loop dropped 13% of
// the WWV sample blocks.
//
// The difference is WHEN. Serving only ever happens while the access point is
// up, and PLAN.md §2 means the WWV sampler is stopped for precisely that whole
// time — ADC2 cannot be read while the WiFi radio is on. So there is no sample
// stream to starve here; there is no measurement in flight to corrupt. The
// worst a rude client can do is delay an NTP reply and a screen redraw, which
// is why NTP is serviced BEFORE this in the loop rather than after.
//
// ── Self-contained by necessity ─────────────────────────────────────────────
//
// The AP has no route to the internet — it is a bare SoftAP whose only other
// service is UDP/123. A stylesheet or font from a CDN would hang until the
// browser gave up. Everything is inline, and the page is streamed in chunks off
// a small stack buffer rather than concatenated into one big String, so serving
// it repeatedly cannot fragment the heap.

#ifdef AIRTIME

#include "Common.h"
#include "Menu.h"

// WebServer.h declares a serveStatic() taking an unqualified `FS&` without
// including the header that names it, so it does not compile on its own.
// Normally FS.h supplies the global alias and nobody notices — but TFT_eSPI
// defines FS_NO_GLOBALS (Processors/TFT_eSPI_ESP32_S3.h) to keep its own
// filesystem handling out of the global namespace, and Common.h pulls TFT_eSPI
// in above. So the alias has to be spelled out here. Nothing in this file
// serves files off a filesystem; this exists purely to satisfy a declaration
// neither library will ever use in this build.
#include <FS.h>
using fs::FS;
#include <WebServer.h>
#include <airtime_core.h>

// Supplied by AirTimeMode.cpp — the live app, or nullptr before setup runs.
const airtime::AirTimeApp *airtimeApp();
uint32_t airtimeRdsAccepted();
uint32_t airtimeRdsRejected();
uint32_t airtimeApFailures();
uint32_t airtimeNtpServed();
int32_t  airtimeRdsTunedKhz();

static WebServer *atServer = nullptr;
static bool atServerUp = false;

// One buffer, reused for every row. 256 is comfortably past the longest line
// below; snprintf truncates rather than overruns if a future row disagrees.
static char atRow[256];

static void atSend(const char *s) { if(atServer) atServer->sendContent(s); }

static void atRowf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(atRow, sizeof(atRow), fmt, ap);
  va_end(ap);
  atSend(atRow);
}

// A labelled value. `warn` colours it amber — used only where the value means
// the device is not doing what it should be.
static void atKv(const char *k, const char *v, bool warn = false)
{
  atRowf("<tr><th>%s</th><td class=\"%s\">%s</td></tr>", k, warn ? "w" : "", v);
}

static const char *atMs(int64_t us)
{
  static char b[32];
  const int64_t ms = us / 1000;
  if(ms > -10000 && ms < 10000) snprintf(b, sizeof(b), "%ld ms", (long)ms);
  else                          snprintf(b, sizeof(b), "%.3f s", (double)us / 1e6);
  return b;
}

static const char *atAge(int64_t us)
{
  static char b[32];
  const long s = (long)(us / 1000000);
  if(s < 90)     snprintf(b, sizeof(b), "%lds", s);
  else if(s < 5400) snprintf(b, sizeof(b), "%ldm", s / 60);
  else           snprintf(b, sizeof(b), "%ldh%02ld", s / 3600, (s % 3600) / 60);
  return b;
}

static const char kStyle[] =
  "<style>"
  ":root{color-scheme:dark light}"
  "body{font:15px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace;"
  "margin:0;padding:16px;background:#111;color:#ddd;max-width:44rem}"
  "h1{font-size:1.1rem;margin:0 0 2px;letter-spacing:.08em;color:#fff}"
  "h2{font-size:.82rem;text-transform:uppercase;letter-spacing:.12em;"
  "color:#7aa;margin:26px 0 6px;border-bottom:1px solid #333;padding-bottom:4px}"
  ".big{font-size:2.6rem;line-height:1.1;font-weight:600;color:#fff}"
  ".sub{font-size:1.1rem;color:#8bd}"
  ".unsync .big,.unsync .sub{color:#fb4}"
  "table{border-collapse:collapse;width:100%;display:block;overflow-x:auto}"
  "th,td{text-align:left;padding:3px 10px 3px 0;vertical-align:top;"
  "white-space:nowrap}"
  "th{color:#8a8;font-weight:400;width:14rem}"
  "td.w{color:#fb4}"
  "td.n{color:#6c8}"
  "p.note{color:#888;font-size:.82rem;margin:22px 0 0;white-space:normal}"
  "@media(prefers-color-scheme:light){body{background:#fff;color:#222}"
  "h1{color:#000}h2{color:#357}.big{color:#000}.sub{color:#25a}"
  "th{color:#575}p.note{color:#666}}"
  "</style>";

static void atHandleRoot()
{
  const airtime::AirTimeApp *app = airtimeApp();

  AirTimeScreen s;
  airtimeScreen(&s);

  atServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
  atServer->send(200, "text/html; charset=utf-8", "");

  atSend("<!doctype html><html><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<meta http-equiv=\"refresh\" content=\"5\">"
         "<title>AirTime</title>");
  atSend(kStyle);
  atSend("</head><body>");

  atRowf("<div class=\"%s\">", s.synced ? "" : "unsync");
  atRowf("<h1>AirTime</h1>");
  atRowf("<div class=\"big\">%s</div>", s.valid ? s.local : "--:--:--");
  atRowf("<div class=\"sub\">%s &nbsp; %s UTC</div>", s.zone, s.clock);
  atSend("</div>");

  if(app == nullptr)
  {
    atSend("<p class=\"note\">The app has not started yet.</p></body></html>");
    atSend("");
    return;
  }

  const airtime::DisplayState st = app->displayState();
  const int64_t mono = (int64_t)millis() * 1000;

  // ── Can this time be trusted, and to what ────────────────────────────────
  atSend("<h2>Confidence</h2><table>");
  atKv("state", !st.clock_valid ? "NO TIME AT ALL"
                : st.synced ? "synchronised"
                : st.ever_synced ? "COASTING - last fix has gone stale"
                                 : "UNSYNCED - restored from memory, unverified",
       !st.synced);
  atKv("uncertainty", atMs(st.uncertainty_us));

  const int64_t pend = app->arbiter().pendingCorrectionUs(mono);
  if(pend != 0)
    atKv("correction still to apply", atMs(pend), true);

  atKv("last verified", st.clock_valid ? atAge(st.since_sync_us) : "never",
       !st.synced);

  char srcs[32];
  airtime::formatSources(st.sources, srcs, sizeof(srcs));
  atKv("sources heard recently", srcs, st.sources == 0);

  atRowf("<tr><th>crystal</th><td>%+.2f ppm learned", app->arbiter().ratePpm());
  const double resid = app->arbiter().drift().residualPpm();
  if(resid < 1e8) atRowf(", %+.2f ppm residual", resid);
  atSend("</td></tr></table>");

  // ── The conversation with each source ────────────────────────────────────
  // Two field failures were invisible without exactly this: three good WWV
  // markers that were all rejected (indistinguishable from "no markers"), and
  // an accepted correction of one hour that was never actually applied
  // (indistinguishable from "the clock is fine").
  atSend("<h2>Last word from each source</h2><table>");
  const airtime::WwvFixDiag &w = app->wwvFixDiag();
  if(w.have)
    atRowf("<tr><th>WWV</th><td class=\"%s\">%s, %s, %s</td></tr>",
           w.accepted ? "n" : "w", atMs(w.offset_us),
           w.corroborated ? "corroborated by a second marker" : "single marker",
           w.accepted ? "ACCEPTED" : "REJECTED and held");
  else
    atKv("WWV", "nothing yet");

  const airtime::RdsFixDiag &r = app->rdsFixDiag();
  if(r.have)
    atRowf("<tr><th>FM RDS</th><td class=\"%s\">%s, %d station%s agreeing, %s</td></tr>",
           r.accepted ? "n" : "w", atMs(r.offset_us), r.stations,
           r.stations == 1 ? "" : "s",
           r.accepted ? "ACCEPTED" : "REJECTED and held");
  else
    atKv("FM RDS", "nothing yet");
  atRowf("<tr><th>RDS groups</th><td>%lu used, %lu discarded as unverifiable</td></tr>",
         (unsigned long)airtimeRdsAccepted(), (unsigned long)airtimeRdsRejected());
  atSend("</table>");

  // ── What the radio is physically doing ───────────────────────────────────
  atSend("<h2>Receiver</h2><table>");
  atKv("dial", s.tuned);
  const char *ph = app->radioMode()   ? "operator has the dial"
                 : app->surveying()   ? "surveying the FM band"
                 : st.phase == airtime::Phase::Acquiring ? "acquiring"
                 : st.phase == airtime::Phase::Listening ? "listening for WWV"
                                                         : "serving time";
  atKv("doing", ph);
  atSend("</table>");

  // ── Which shortwave bands actually deliver here ──────────────────────────
  atSend("<h2>WWV bands</h2><table>"
         "<tr><th>band</th><td>windows</td><td>markers</td><td>best SNR</td></tr>");
  const airtime::Scheduler &sc = app->scheduler();
  for(size_t i = 0; i < sc.bandCount(); i++)
  {
    const airtime::BandStats &b = sc.bandStats(i);
    atRowf("<tr><th>%ld kHz%s</th><td>%d</td><td class=\"%s\">%d</td><td>%.1f</td></tr>",
           (long)b.khz, b.khz == sc.currentBandKhz() ? " *" : "",
           b.attempts, b.successes > 0 ? "n" : "", b.successes, (double)b.best_snr);
  }
  atSend("</table>");

  // ── The detector's own view ──────────────────────────────────────────────
  // flr and pk are what decide whether a beep is heard at all; a jammed band
  // inflates the floor and buries the marker, which is why this is per-band
  // in the log and why the band table above sits directly over it.
  const airtime::WwvMarkerDiag &m = app->wwvMarker().diag();
  atSend("<h2>Minute-marker detector</h2><table>");
  atRowf("<tr><th>noise floor / peak</th><td>%.2e / %.2e</td></tr>",
         (double)m.noise_floor, (double)m.max_power);
  atRowf("<tr><th>bursts detected</th><td>%lu</td></tr>", (unsigned long)m.tone_starts);
  atRowf("<tr><th>last / longest burst</th><td>%ld ms / %ld ms</td></tr>",
         (long)(m.last_tone_us / 1000), (long)(m.longest_tone_us / 1000));
  atRowf("<tr><th>rejected by length</th><td>%lu short, %lu long</td></tr>",
         (unsigned long)m.rejected_short, (unsigned long)m.rejected_long);
  atRowf("<tr><th>minute markers accepted</th><td class=\"%s\">%lu</td></tr>",
         m.markers ? "n" : "", (unsigned long)m.markers);
  atSend("</table>");

  // ── What each FM station has been caught doing ───────────────────────────
  // A station that is reliably late is still useful once the lateness is known
  // and subtracted; this table is that knowledge, and it is why a broadcaster
  // with a sloppy clock does not have to be thrown away.
  const airtime::StationBiasTable &bias = app->stationBias();
  if(bias.count() > 0)
  {
    atSend("<h2>FM stations, and how late each one runs</h2><table>"
           "<tr><th>PI code</th><td>bias</td><td>samples</td></tr>");
    for(size_t i = 0; i < bias.count(); i++)
    {
      const airtime::StationBias &b = bias.at(i);
      atRowf("<tr><th>%04X</th><td>%s</td><td>%d</td></tr>",
             b.pi, atMs(b.bias_us), b.samples);
    }
    atSend("</table>");
  }

  // ── Network ──────────────────────────────────────────────────────────────
  atSend("<h2>Time service</h2><table>");
  atKv("clients", s.clients);
  atRowf("<tr><th>NTP requests answered</th><td>%lu</td></tr>",
         (unsigned long)airtimeNtpServed());
  const uint32_t af = airtimeApFailures();
  if(af)
    atRowf("<tr><th>access point failures</th><td class=\"w\">%lu</td></tr>",
           (unsigned long)af);
  atSend("</table>");

  // ── Nets ─────────────────────────────────────────────────────────────────
  if(s.net[0])
  {
    atSend("<h2>On the air</h2><table>");
    atKv("net", s.net);
    atSend("</table>");
  }

  // The §2 note. Without it, the hourly outage reads as a crash — the page
  // simply stops answering for a few minutes and the AP disappears with it.
  atSend("<p class=\"note\">Refreshes every 5 s. "
         "This page and the access point both go away during a WWV listening "
         "window &mdash; the ESP32 cannot read the audio tap on ADC2 while its "
         "WiFi radio is powered, so the radio is switched off to listen. "
         "That is the device working correctly, not a fault; it comes back by "
         "itself when the window closes.</p>");

  atSend("</body></html>");
  atSend("");   // terminate the chunked response
}

void airtimeWebService(bool wifi_up)
{
  if(wifi_up && !atServerUp)
  {
    if(atServer == nullptr)
    {
      atServer = new WebServer(80);
      atServer->on("/", atHandleRoot);
      atServer->onNotFound(atHandleRoot);   // any path, one page
    }
    atServer->begin();
    atServerUp = true;
  }
  else if(!wifi_up && atServerUp)
  {
    // The socket has to be released before the radio goes down, or the next
    // bringUp() inherits a listener bound to an interface that no longer
    // exists. Same discipline as the UDP socket in Esp32WiFiControl.
    atServer->close();
    atServerUp = false;
  }

  if(atServerUp) atServer->handleClient();
}

#endif  // AIRTIME
