#!/usr/bin/env python3
"""Measure Milestone 5: does the WSJT-X DT column cluster near zero?

    tools/dt_report.py ~/Library/Application\\ Support/WSJT-X/ALL.TXT
    tools/dt_report.py ALL.TXT --since 2026-07-27T18:00 --band 14.074
    tools/dt_report.py ALL.TXT --split 2026-07-27T20:15   # before/after a change

DT is what WSJT-X computes as the time offset of a decoded signal against the
receiver's own clock. Every other station on the band is, collectively, a
reference: if this radio's clock is right, their DT values straddle zero. If it
is half a second slow, every one of them lands near +0.5 s.

That makes DT the only end-to-end check of the whole chain -- receiver, marker
detector, arbiter, NTP, the laptop's own client -- against a population of
strangers who have no idea they are being used as a standard. It is the test
this project exists to pass, and it cannot be run on a bench.

WHAT TO LOOK FOR

  median   the systematic error. This is the number. Near zero means the clock
           is right; a consistent offset means it is not, by that much.
  spread   the IQR (middle 50%) of the population. This is THEIR scatter plus
           propagation, not this radio's -- it will not shrink below about
           0.2 s no matter how good the clock is, and it is not a fault.
  drift    median DT of the first quarter of the session vs the last. A clock
           losing time shows up here as a slope even when the median looks fine.

A caution on reading a single evening: DT is measured against other people's
clocks, and some of them are wrong. The median is used throughout rather than
the mean for exactly that reason -- a handful of badly-set stations move a mean
and cannot move a median.
"""
import argparse
import re
import sys
from datetime import datetime

# WSJT-X ALL.TXT has drifted across versions. Both of these appear in the wild:
#
#   250727_143000    14.074 Rx FT8    -12  0.2 1580 CQ K1ABC FN42
#   250727_143000 14.074000 Rx FT8    -12  0.2 1580 CQ K1ABC FN42
#
# and JT/JS8 lines share the shape. Rather than pin a version, take the columns
# positionally after the timestamp and be strict only about what must be true:
# a 13-character timestamp, a decodable frequency, Rx, and a numeric SNR and DT.
LINE = re.compile(
    r"^(?P<ts>\d{6}_\d{6})\s+"
    r"(?P<mhz>\d+\.\d+)\s+"
    r"(?P<dir>Rx|Tx)\s+"
    r"(?P<mode>\S+)\s+"
    r"(?P<snr>[-+]?\d+)\s+"
    r"(?P<dt>[-+]?\d+\.\d+)\s+"
    r"(?P<hz>\d+)\s+"
    r"(?P<msg>.*)$"
)


def parse_ts(s):
    # YYMMDD_HHMMSS. WSJT-X writes these in UTC.
    return datetime.strptime(s, "%y%m%d_%H%M%S")


def parse_when(s):
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d", "%H:%M"):
        try:
            v = datetime.strptime(s, fmt)
            return v
        except ValueError:
            pass
    raise SystemExit("cannot read a time from %r "
                     "(try 2026-07-27T20:15)" % s)


def median(xs):
    if not xs:
        return None
    v = sorted(xs)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def quartiles(xs):
    if len(xs) < 4:
        return (None, None)
    v = sorted(xs)
    return (median(v[: len(v) // 2]), median(v[(len(v) + 1) // 2:]))


def load(path, since=None, until=None, band=None, mode=None):
    rows, seen, skipped = [], 0, 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                skipped += 1
                continue
            if m.group("dir") != "Rx":
                continue
            seen += 1
            try:
                ts = parse_ts(m.group("ts"))
            except ValueError:
                continue
            if since and ts < since:
                continue
            if until and ts > until:
                continue
            mhz = float(m.group("mhz"))
            if band is not None and abs(mhz - band) > 0.01:
                continue
            if mode and m.group("mode").upper() != mode.upper():
                continue
            rows.append((ts, mhz, m.group("mode"), float(m.group("dt")),
                         int(m.group("snr")), m.group("msg")))
    return rows, seen, skipped


def histogram(dts, lo=-1.5, hi=1.5, bins=25, width=46):
    """Where the population actually sits. A clock error is a shifted hump, not
    a wider one -- the shape tells you which of the two you have."""
    step = (hi - lo) / bins
    counts = [0] * bins
    under = over = 0
    for d in dts:
        if d < lo:
            under += 1
        elif d >= hi:
            over += 1
        else:
            counts[min(bins - 1, int((d - lo) / step))] += 1
    peak = max(counts) if counts else 0
    out = []
    if under:
        out.append("   <%-5.1f %s %d" % (lo, " " * width, under))
    for i, c in enumerate(counts):
        centre = lo + step * (i + 0.5)
        bar = "#" * int(round(width * c / peak)) if peak else ""
        mark = " <- 0" if abs(centre) < step / 2 else ""
        out.append("  %+6.2f %-*s %4d%s" % (centre, width, bar, c, mark))
    if over:
        out.append("   >%-5.1f %s %d" % (hi, " " * width, over))
    return "\n".join(out)


def summarise(label, rows):
    dts = [r[3] for r in rows]
    if not dts:
        print("%s: no decodes" % label)
        return None
    med = median(dts)
    q1, q3 = quartiles(dts)
    inside = sum(1 for d in dts if abs(d) <= 0.5)
    print("%s" % label)
    print("  decodes        %d" % len(dts))
    print("  median DT      %+.2f s   <- the systematic error" % med)
    if q1 is not None:
        print("  middle 50%%     %+.2f .. %+.2f s  (spread %.2f s)"
              % (q1, q3, q3 - q1))
    print("  within +/-0.5s %d of %d  (%.0f%%)"
          % (inside, len(dts), 100.0 * inside / len(dts)))

    # Drift across the session: first quarter against last quarter. A clock
    # that is losing or gaining shows a slope here even when the median is
    # respectable, because the median averages the two ends together.
    if len(dts) >= 20:
        q = len(rows) // 4
        early, late = median([r[3] for r in rows[:q]]), median([r[3] for r in rows[-q:]])
        print("  first vs last  %+.2f -> %+.2f s  (drift %+.2f s over %s)"
              % (early, late, late - early, rows[-1][0] - rows[0][0]))
    return med


def main():
    ap = argparse.ArgumentParser(
        description="Milestone 5 acceptance: DT distribution from WSJT-X ALL.TXT",
        epilog="Typical macOS path: "
               "~/Library/Application Support/WSJT-X/ALL.TXT")
    ap.add_argument("logfile")
    ap.add_argument("--since", help="ignore decodes before this UTC time")
    ap.add_argument("--until", help="ignore decodes after this UTC time")
    ap.add_argument("--split", help="report separately before and after this "
                                    "UTC time -- use it to compare the laptop's "
                                    "own clock against AirTime's")
    ap.add_argument("--band", type=float, help="MHz, e.g. 14.074")
    ap.add_argument("--mode", help="FT8, FT4, JS8 ...")
    ap.add_argument("--no-histogram", action="store_true")
    a = ap.parse_args()

    since = parse_when(a.since) if a.since else None
    until = parse_when(a.until) if a.until else None
    split = parse_when(a.split) if a.split else None

    try:
        rows, seen, skipped = load(a.logfile, since, until, a.band, a.mode)
    except OSError as e:
        raise SystemExit("cannot read %s: %s" % (a.logfile, e))

    if not rows:
        print("No decodes matched.")
        print("  %d Rx lines were parsed from the file, %d lines did not look "
              "like decodes." % (seen, skipped))
        if seen and (since or until or a.band or a.mode):
            print("  Every parsed decode was excluded by a filter -- check "
                  "--since/--until/--band/--mode.")
        return 1

    span = "%s .. %s UTC" % (rows[0][0].strftime("%Y-%m-%d %H:%M"),
                             rows[-1][0].strftime("%H:%M"))
    bands = sorted({r[1] for r in rows})
    print("AirTime -- Milestone 5 DT report")
    print("%s   %s" % (span, ", ".join("%.3f MHz" % b for b in bands)))
    print()

    if split:
        before = [r for r in rows if r[0] < split]
        after = [r for r in rows if r[0] >= split]
        mb = summarise("BEFORE %s" % split.strftime("%H:%M UTC"), before)
        print()
        ma = summarise("AFTER  %s" % split.strftime("%H:%M UTC"), after)
        if mb is not None and ma is not None:
            print()
            print("  change in median DT  %+.2f -> %+.2f s  (%+.2f s)"
                  % (mb, ma, ma - mb))
        rows_for_hist = after or rows
    else:
        summarise("Session", rows)
        rows_for_hist = rows

    if not a.no_histogram:
        print()
        print("DT distribution (s):")
        print(histogram([r[3] for r in rows_for_hist]))

    print()
    med = median([r[3] for r in rows_for_hist])
    if abs(med) <= 0.20:
        print("VERDICT: PASS -- median DT %+.2f s. The clock is not the "
              "limiting factor." % med)
    elif abs(med) <= 0.50:
        print("VERDICT: usable but off by %+.2f s. Inside FT8's tolerance, "
              "outside this project's target." % med)
        print("         Check the status page: is `correction still to apply` "
              "non-zero, or does one FM station dominate?")
    else:
        print("VERDICT: FAIL -- median DT %+.2f s is a real clock error." % med)
        print("         Grab the serial log's fix[] line and the status page "
              "before power-cycling; both say WHY, and neither survives a reboot.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
