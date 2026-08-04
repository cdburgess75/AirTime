#!/usr/bin/env python3
"""Build the browser demo of the phone app from the firmware source.

    python3 tools/make_demo.py      # -> docs/demo/index.html

The app that runs on the radio is a C string literal in AirTimeWeb.cpp, served
off the device's own access point. That is the only place it can normally be
seen, which makes it impossible to link to and impossible to review without a
radio in hand.

So the demo is GENERATED from that same literal rather than written twice. It
cannot drift from what the device serves, because there is only one copy: this
script lifts it out, swaps fetch('/api') for a simulated radio, and stamps the
page as a demo so nobody mistakes it for a live device.
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "firmware" / "ats-mini" / "ats-mini" / "AirTimeWeb.cpp"
OUT = ROOT / "docs" / "demo" / "index.html"

src = SRC.read_text()
try:
    body = src.split("static const char kApp[] =", 1)[1]
    body = body.split(";\n\nstatic void atHandleApp", 1)[0]
except IndexError:
    sys.exit("make_demo: could not find kApp[] in %s" % SRC)

# Join the adjacent C string literals and undo the C escaping.
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
html = "".join(parts)
html = (html.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t")
            .replace("\\u2019", "’").replace("\\\\", "\\"))

# A radio that is always in range. Same shape /api really returns — if the two
# ever disagree the demo breaks visibly, which is the point of generating it.
STUB = """<script>
/* DEMO ONLY. There is no radio here; this stands in for one so the interface
   can be seen without hardware. On a real device every value below arrives
   from /api, served by the radio over its own access point. */
(function(){
  var boot = Date.now();
  window.fetch = function(u){
    if (String(u).indexOf('/api') < 0)
      return Promise.resolve({ json: function(){ return {}; } });
    var up = Math.floor((Date.now()-boot)/1000);
    return Promise.resolve({ json: function(){ return {
      v:"demo", val:1, syn:1, utc:Date.now(), tzo:-18000, zone:"CDT",
      unc:"120 ms", age:"26s", src:"RDS+WWV", ppm:"+12.07 ppm",
      net:"MMSN 14300 in 2h10",
      rx:{ d:"FM 89.3 RDS", fm:1, r:47, s:22, g:"serving time" },
      ntp:{ c:2, a:1841 }, eibi:8142,
      up:Math.floor(up/3600)+"h "+("0"+Math.floor(up/60)%60).slice(-2)+"m",
      cyc:{ i:1, n:9, p:15000, l:["Off","FT8","FT4","FT2","JS8 Normal",
            "JS8 Fast","JS8 Turbo","JS8 Slow","JT65/JT9","WSPR"] }
    }; } });
  };
})();
</script>"""

BANNER = """<div style="position:fixed;left:0;right:0;bottom:0;z-index:9;
background:#2f6df6;color:#fff;font:600 12px/1.35 -apple-system,system-ui,sans-serif;
padding:9px 14px;text-align:center">
DEMO &mdash; simulated data, no radio attached.
<a href="https://github.com/cdburgess75/AirTime" style="color:#fff">See the project &rarr;</a>
</div>"""

html = html.replace("<script>", STUB + "<script>", 1)
html = html.replace("</body>", BANNER + "</body>", 1)
html = html.replace("<title>AirTime</title>", "<title>AirTime &mdash; demo</title>", 1)

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(html)
print("%s  (%d bytes, generated from %s)" % (OUT, len(html), SRC.name))
