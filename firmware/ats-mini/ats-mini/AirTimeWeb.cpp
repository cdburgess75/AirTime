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
#include "EIBI.h"   // eibiEntryCount — the app reports what the device HOLDS

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
#include "AirTimeIcon.h"

// Supplied by AirTimeMode.cpp — the live app, or nullptr before setup runs.
const airtime::AirTimeApp *airtimeApp();
uint32_t airtimeRdsAccepted();
uint32_t airtimeRdsRejected();
uint32_t airtimeRdsNotFm();
uint32_t airtimeRdsNoSync();
uint32_t airtimeRdsEmpty();
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


// ── The phone app ───────────────────────────────────────────────────────────
//
// Add to Home Screen on an iPhone joined to the AirTime access point and this
// becomes a full-screen app: no browser chrome, its own icon, the clock in the
// size a clock deserves. The apple-mobile-web-app-* meta tags below are what
// iOS keys on; the icon is a real PNG because iOS ignores SVG and data: URIs
// for apple-touch-icon.
//
// ── What it deliberately does NOT have ──────────────────────────────────────
//
// A service worker, which is what would let it open with the last known state
// while the radio is off the air. Service workers require a secure context and
// this is plain HTTP on 192.168.4.1 — there is no certificate to be had for an
// IP address on an island network, so the browser will not register one. The
// app therefore needs the access point up to load at all, and says so plainly
// when it cannot reach the radio rather than spinning forever.
//
// ── Why it interpolates instead of polling fast ─────────────────────────────
//
// A clock that ticks once per HTTP round trip looks broken. /api is polled
// every 2 s and the browser advances the display itself between polls from the
// device's UTC and its own monotonic clock, resyncing on each reply. The
// phone's own wall clock is never consulted — that would defeat the entire
// point of the device.
static const char kApp[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>AirTime</title>"
"<meta name=apple-mobile-web-app-capable content=yes>"
"<meta name=mobile-web-app-capable content=yes>"
"<meta name=apple-mobile-web-app-status-bar-style content=black-translucent>"
"<meta name=apple-mobile-web-app-title content=AirTime>"
"<meta name=theme-color content=#0d1220>"
"<link rel=apple-touch-icon href=/icon.png>"
"<link rel=icon href=/icon.png>"
"<link rel=manifest href=/manifest.json>"
"<style>"
"*{box-sizing:border-box}"
"body{margin:0;background:#0d1220;color:#e9eef7;"
"font:16px/1.4 -apple-system,BlinkMacSystemFont,'SF Pro Text',system-ui,sans-serif;"
"padding:max(14px,env(safe-area-inset-top)) 14px max(14px,env(safe-area-inset-bottom));"
"-webkit-text-size-adjust:100%;-webkit-tap-highlight-color:transparent}"
".hd{display:flex;align-items:baseline;justify-content:space-between;opacity:.5;"
"font-size:11px;letter-spacing:.14em;text-transform:uppercase}"
".clk{font:600 clamp(52px,19vw,88px)/1 ui-monospace,SFMono-Regular,Menlo,monospace;"
"font-variant-numeric:tabular-nums;margin:6px 0 0;letter-spacing:-.02em}"
".utc{font:15px/1 ui-monospace,SFMono-Regular,Menlo,monospace;opacity:.6;margin-top:6px}"
".pill{display:inline-block;margin-top:12px;padding:5px 12px;border-radius:999px;"
"font-size:12px;font-weight:600;letter-spacing:.06em;background:#16351f;color:#5fd08a}"
".pill.w{background:#3a2a10;color:#ffb02e}"
".bar{margin-top:16px;height:44px;border-radius:12px;background:#161d31;"
"position:relative;overflow:hidden}"
".bar>i{position:absolute;inset:0 auto 0 0;width:0;background:#2f6df6;"
"transition:width .1s linear}"
".bar.odd>i{background:#c86bf0}.bar.w>i{background:#7a5a1e}"
".bar>b,.bar>u{position:absolute;top:13px;font-size:14px;font-weight:600;"
"text-decoration:none;font-variant-numeric:tabular-nums}"
".bar>b{left:14px}.bar>u{right:14px}"
"h2{font-size:11px;letter-spacing:.14em;text-transform:uppercase;opacity:.45;"
"margin:26px 0 8px;font-weight:600}"
".card{background:#141b2d;border-radius:14px;padding:2px 14px}"
".r{display:flex;justify-content:space-between;gap:12px;padding:9px 0;"
"border-bottom:1px solid #1e2740;font-size:14px}"
".r:last-child{border-bottom:0}"
".r>span:first-child{opacity:.55;flex:0 0 auto}"
".r>span:last-child{text-align:right;font-variant-numeric:tabular-nums}"
".w{color:#ffb02e}"
".seg{display:flex;flex-wrap:wrap;gap:7px;margin-top:2px}"
".seg button{flex:1 1 auto;min-width:74px;padding:11px 8px;border:0;border-radius:10px;"
"background:#1c2540;color:#cdd6e8;font:600 13px/1 inherit;-webkit-appearance:none}"
".seg button.on{background:#2f6df6;color:#fff}"
".seg button:active{opacity:.6}"
"p{opacity:.42;font-size:12px;line-height:1.5}"
"#off{position:fixed;inset:0;background:#0d1220ee;display:none;place-items:center;"
"text-align:center;padding:32px;backdrop-filter:blur(3px)}"
"#off.on{display:grid}"
"#off div{max-width:22rem}#off h3{font-size:17px;margin:0 0 10px;color:#ffb02e}"
"</style></head><body>"
"<div class=hd><span>AirTime</span><span id=ver></span></div>"
"<div class=clk id=clk>--:--:--</div>"
"<div class=utc id=utc>&nbsp;</div>"
"<span class=pill id=pill>starting</span>"
"<div class=bar id=bar style=display:none><i id=barf></i><b id=barn></b><u id=bart></u></div>"
"<h2>Cycle</h2><div class=seg id=seg></div>"
"<h2>Clock</h2><div class=card id=cconf></div>"
"<h2>Receiver</h2><div class=card id=crx></div>"
"<h2>Serving</h2><div class=card id=cntp></div>"
"<p id=note></p>"
"<div id=off><div><h3>Radio is off the air</h3>"
"<p style=opacity:.7>The access point goes down while the radio listens for WWV "
"&mdash; the ESP32 cannot read the audio tap and run its WiFi radio at the same "
"time. It comes back by itself, usually within a couple of minutes.</p></div></div>"
"<script>"
"var D=null,T0=0,U0=0,P=0;"
"function q(i){return document.getElementById(i)}"
"function pad(n,w){n=String(n);while(n.length<(w||2))n='0'+n;return n}"
"function hms(ms){var d=new Date(ms);return pad(d.getUTCHours())+':'+pad(d.getUTCMinutes())+':'+pad(d.getUTCSeconds())}"
"function rows(el,a){var h='';for(var i=0;i<a.length;i++)h+='<div class=r><span>'+a[i][0]+"
"'</span><span class=\"'+(a[i][2]?'w':'')+'\">'+a[i][1]+'</span></div>';el.innerHTML=h}"
"function seg(){if(!D)return;var h='';for(var i=0;i<=D.cyc.n;i++)"
"h+='<button onclick=setc('+i+') class=\"'+(i==D.cyc.i?'on':'')+'\">'+D.cyc.l[i]+'</button>';"
"q('seg').innerHTML=h}"
"function setc(i){fetch('/set?cycle='+i).then(function(){return pull()})}"
"function paint(){"
"if(!D){requestAnimationFrame(paint);return}"
"var now=U0+(performance.now()-T0);"                     /* device UTC, ms */
"q('clk').textContent=D.val?hms(now+D.tzo*1000):'--:--:--';"
"q('utc').innerHTML=D.val?(D.zone+' &nbsp;&middot;&nbsp; '+hms(now)+' UTC'):'&nbsp;';"
"if(P>0&&D.val){var b=q('bar');b.style.display='';"
"var into=((now%P)+P)%P,slot=Math.floor(now/P);"
"q('barf').style.width=(into/P*100)+'%';"
"b.className='bar'+(!D.syn?' w':(slot%2?' odd':''));"
"q('barn').textContent=D.cyc.l[D.cyc.i]+' '+(P%1000?(P/1000).toFixed(1):P/1000)+'s';"
"var rem=(P-into)/1000;q('bart').textContent='T-'+rem.toFixed(rem<10?2:1)+'s';}"
"else q('bar').style.display='none';"
"requestAnimationFrame(paint)}"
"function apply(j){D=j;U0=j.utc;T0=performance.now();P=j.cyc.p;"
"q('off').className='';q('ver').textContent=j.v;"
"var p=q('pill');p.textContent=j.val?(j.syn?'SYNCED '+j.unc:'UNSYNCED'):'NO TIME YET';"
"p.className='pill'+(j.syn?'':' w');"
"rows(q('cconf'),[['uncertainty',j.unc,!j.syn],['sources',j.src,j.src=='none'],"
"['last verified',j.age,!j.syn],['crystal',j.ppm],['on the air',j.net||'-']]);"
"rows(q('crx'),[['dial',j.rx.d],['chip mode',j.rx.fm?'FM':'AM/SSB'],"
"['signal','RSSI '+j.rx.r+' SNR '+j.rx.s,j.rx.r<10],['doing',j.rx.g],"
"['schedule',j.eibi?(j.eibi+' entries'):'NOT INSTALLED',!j.eibi]]);"
"rows(q('cntp'),[['NTP','192.168.4.1:123'],['clients',j.ntp.c],"
"['requests answered',j.ntp.a],['uptime',j.up]]);"
"seg();"
"q('note').textContent='Polls every 2 s; the clock runs from the radio\\u2019s own time '"
"+'between polls, never the phone\\u2019s. Full diagnostics at /status.'}"
"function pull(){return fetch('/api',{cache:'no-store'}).then(function(r){return r.json()})"
".then(apply).catch(function(){q('off').className='on'})}"
"pull();setInterval(pull,2000);requestAnimationFrame(paint);"
"</script></body></html>";

static void atHandleApp()
{
  atServer->sendHeader("Cache-Control", "no-store");
  atServer->send(200, "text/html; charset=utf-8", kApp);
}

static void atHandleIcon()
{
  atServer->sendHeader("Cache-Control", "max-age=86400");
  atServer->send_P(200, "image/png", (const char*)kAirTimeIcon, kAirTimeIconLen);
}

static void atHandleManifest()
{
  atServer->send(200, "application/manifest+json",
    "{\"name\":\"AirTime\",\"short_name\":\"AirTime\",\"display\":\"standalone\","
    "\"background_color\":\"#0d1220\",\"theme_color\":\"#0d1220\",\"start_url\":\"/\","
    "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}");
}

// Live state, small and flat. Everything the app draws comes from here; the
// page itself is static and cacheable.
static void atHandleApi()
{
  const airtime::AirTimeApp *app = airtimeApp();
  AirTimeScreen s;
  airtimeScreen(&s);

  atServer->sendHeader("Cache-Control", "no-store");
  atServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
  atServer->send(200, "application/json", "");

  if(app == nullptr) { atSend("{\"v\":\"" AIRTIME_VERSION "\",\"val\":0}"); atSend(""); return; }

  const airtime::DisplayState st = app->displayState();
  char srcs[32];
  airtime::formatSources(st.sources, srcs, sizeof(srcs));

  // utc as milliseconds: 1.8e12 today, comfortably inside a JS safe integer,
  // and the app only ever needs millisecond resolution to draw with.
  atRowf("{\"v\":\"%s\",\"val\":%d,\"syn\":%d,\"utc\":%lld,\"tzo\":%d,\"zone\":\"%s\",",
         AIRTIME_VERSION, st.clock_valid ? 1 : 0, st.synced ? 1 : 0,
         (long long)(st.utc_us / 1000), atLocalOffsetS(), s.zone);
  atRowf("\"unc\":\"%s\",\"age\":\"%s\",\"src\":\"%s\",",
         atMs(st.uncertainty_us), st.clock_valid ? atAge(st.since_sync_us) : "never",
         srcs);
  atRowf("\"ppm\":\"%+.2f ppm\",\"net\":\"%s\",", app->arbiter().ratePpm(), s.net);
  atRowf("\"rx\":{\"d\":\"%s\",\"fm\":%d,\"r\":%d,\"s\":%d,\"g\":\"%s\"},",
         s.tuned, rx.isCurrentTuneFM() ? 1 : 0, (int)rssi, (int)snr,
         app->cwMode()    ? "CW copy" :
         app->radioMode() ? "operator has the dial" :
         app->surveying() ? "surveying the FM band" :
         st.phase == airtime::Phase::Listening ? "listening for WWV" :
         st.phase == airtime::Phase::Acquiring ? "acquiring" : "serving time");
  atRowf("\"ntp\":{\"c\":%d,\"a\":%lu},\"eibi\":%d,",
         st.ntp_clients, (unsigned long)airtimeNtpServed(), eibiEntryCount());

  const uint32_t up = millis() / 1000;
  atRowf("\"up\":\"%luh %02lum\",", (unsigned long)(up / 3600),
         (unsigned long)((up / 60) % 60));

  // The cycle picker's whole model in one object: current index, how many
  // modes exist, the period to animate, and every label.
  atRowf("\"cyc\":{\"i\":%d,\"n\":%d,\"p\":%ld,\"l\":[",
         atCycleIdx(), atCycleCount(), atCyclePeriodMs(atCycleIdx()));
  for(int i = 0 ; i <= atCycleCount() ; i++)
    atRowf("%s\"%s\"", i ? "," : "", atCycleName(i));
  atSend("]}}");
  atSend("");
}

// Settings the app is allowed to change. Deliberately only the ones that
// cannot take the access point down under the operator's feet: switching to
// CW or the waterfall, or forcing a listen window, kills the WiFi radio and
// with it this connection (PLAN.md §2). Those stay on the device's own menu,
// where the person pressing the button is looking at the screen.
static void atHandleSet()
{
  if(atServer->hasArg("cycle"))
    atSetCycleIdx(atServer->arg("cycle").toInt());
  atServer->sendHeader("Cache-Control", "no-store");
  atServer->send(200, "text/plain", "ok");
}

static void atHandleStatus()
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
  // Stage-by-stage, so "0 used" names its own cause: not-FM polls mean the
  // chip is not where the software believes; no-sync with a strong S-meter
  // means the RDS decoder has nothing to lock to; empty is healthy waiting.
  atRowf("<tr><th>RDS pipeline</th><td class=\"%s\">"
         "%lu not-FM, %lu no-sync, %lu empty</td></tr>",
         (airtimeRdsNotFm() > 10 || airtimeRdsNoSync() > 100) ? "w" : "",
         (unsigned long)airtimeRdsNotFm(), (unsigned long)airtimeRdsNoSync(),
         (unsigned long)airtimeRdsEmpty());
  atSend("</table>");

  // ── What the radio is physically doing ───────────────────────────────────
  atSend("<h2>Receiver</h2><table>");
  atKv("dial", s.tuned);
  // The dial row above is OUR CACHED CLAIM about the chip. These two are the
  // chip's own story, and they exist because a field UNSYNCED came down to
  // exactly this gap: zero RDS groups while the dial row confidently said
  // "FM 89.9". "chip mode" says whether the tune actually took; "signal" says
  // whether there is anything on the frequency to decode. Between them, "no
  // signal here" and "chip is not where we think" stop being guesses.
  atKv("chip mode", rx.isCurrentTuneFM() ? "FM" : "AM/SSB",
       !rx.isCurrentTuneFM() && !app->radioMode() && !app->cwMode());
  // ...and this is the REST OF THE FIRMWARE's story about the chip. stock's
  // currentMode picks the S-meter's dBuV->S curve, the squelch slot and the
  // AGC table, so when it disagrees with the tuner the meter below is drawn
  // on the wrong scale and the numbers mean nothing. A field photo showed six
  // bars beside zero RDS groups purely because of this. Kept as a tripwire.
  {
    static const char *kModeTxt[] = {"FM", "LSB", "USB", "AM"};
    const bool agrees = ((currentMode == FM) == rx.isCurrentTuneFM());
    atRowf("<tr><th>stock mode</th><td class=\"%s\">%s%s</td></tr>",
           agrees ? "" : "w",
           currentMode < 4 ? kModeTxt[currentMode] : "?",
           agrees ? "" : " &mdash; DISAGREES with tuner, meter is unreliable");
  }
  atRowf("<tr><th>signal</th><td class=\"%s\">RSSI %d dBuV, SNR %d dB</td></tr>",
         rssi < 10 ? "w" : "", rssi, snr);
  atRowf("<tr><th>AGC</th><td class=\"%s\">%s, attenuator %d</td></tr>",
         disableAgc ? "w" : "", disableAgc ? "OFF" : "on", (int)agcNdx);
  const char *ph = app->cwMode()      ? "CW copy - NTP is off the air"
                 : app->radioMode()   ? "operator has the dial"
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
      atServer->on("/", atHandleApp);
      atServer->on("/status", atHandleStatus);
      atServer->on("/api", atHandleApi);
      atServer->on("/set", atHandleSet);
      atServer->on("/icon.png", atHandleIcon);
      atServer->on("/manifest.json", atHandleManifest);
      atServer->onNotFound(atHandleApp);
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
