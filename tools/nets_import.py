#!/usr/bin/env python3
"""Turn a net schedule into the C table AirTime compiles in.

    tools/nets_import.py nets.json  > snippet.c
    tools/nets_import.py nets.csv --region "Gulf Coast"

Accepts JSON (a list of objects, or {"nets": [...]}) or CSV with a header row.
Field names are matched loosely, so most hand-made or exported files work
without editing:

    name          name | net | title
    khz           khz  | freq | frequency        (MHz values are converted)
    mode          mode                            (USB/LSB/CW/AM/FM)
    days          days | day | schedule           ("daily", "weekdays",
                                                   "Mon,Wed,Fri", "MWF", ...)
    start         start | time | utc              ("1200", "12:00", "1200Z")
    duration_min  duration | mins | length        (default 60)
    region        region                          (optional, for filtering)

Times are UTC, deliberately: a net at 1200Z is at 1200Z in March and in July,
which is why airtime/nets.h keys off UTC and leaves local time to the display.
If your source is in LOCAL time, convert before importing — this script will
not guess a zone, because guessing wrong is silently an hour out.
"""
import csv, json, re, sys

DAYS = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
MASK = {d: 1 << i for i, d in enumerate(DAYS)}
MODES = {"usb": "Usb", "lsb": "Lsb", "cw": "Cw", "am": "Am", "fm": "Fm"}


def pick(row, *names, default=None):
    for n in names:
        for k, v in row.items():
            if k and k.strip().lower() == n and str(v).strip():
                return str(v).strip()
    return default


def parse_days(s):
    if not s:
        return "kDaily"
    t = s.strip().lower()
    if t in ("daily", "everyday", "every day", "all"):
        return "kDaily"
    if t in ("weekdays", "mon-fri", "m-f"):
        return "kWeekdays"
    bits = [d for d in DAYS if d in t]
    if not bits and re.fullmatch(r"[mtwrfsu]+", t):      # "MWF" style
        short = {"u": "sun", "m": "mon", "t": "tue", "w": "wed",
                 "r": "thu", "f": "fri", "s": "sat"}
        bits = [short[c] for c in t if c in short]
    if not bits:
        raise ValueError(f"cannot read days: {s!r}")
    return " | ".join("k" + d.capitalize() for d in bits)


def parse_time(s):
    if not s:
        raise ValueError("missing start time")
    t = re.sub(r"[^0-9:]", "", str(s))
    if ":" in t:
        h, m = t.split(":")[:2]
    elif len(t) == 4:
        h, m = t[:2], t[2:]
    elif len(t) <= 2:
        h, m = t, "0"
    else:
        raise ValueError(f"cannot read time: {s!r}")
    h, m = int(h), int(m)
    if not (0 <= h < 24 and 0 <= m < 60):
        raise ValueError(f"time out of range: {s!r}")
    return h * 60 + m


def parse_khz(s):
    v = float(re.sub(r"[^0-9.]", "", str(s)))
    return int(round(v * 1000)) if v < 1000 else int(round(v))   # MHz or kHz


def load(path):
    if path.endswith(".json"):
        d = json.load(open(path))
        return d.get("nets", d) if isinstance(d, dict) else d
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    region = None
    if "--region" in sys.argv:
        region = sys.argv[sys.argv.index("--region") + 1]

    out, skipped = [], 0
    for row in load(sys.argv[1]):
        r = pick(row, "region")
        if region and r and r.lower() != region.lower():
            continue
        try:
            name = pick(row, "name", "net", "title")
            khz = parse_khz(pick(row, "khz", "freq", "frequency"))
            mode = MODES[pick(row, "mode", default="usb").lower()]
            days = parse_days(pick(row, "days", "day", "schedule"))
            start = parse_time(pick(row, "start", "time", "utc"))
            dur = int(float(pick(row, "duration_min", "duration", "mins",
                                 "length", default="60")))
        except Exception as e:                       # noqa: BLE001
            print(f"// SKIPPED {row}: {e}", file=sys.stderr)
            skipped += 1
            continue
        out.append(f'  {{"{name[:15]}", {khz}, airtime::NetMode::{mode}, '
                   f'{days}, {start}, {dur}}},')

    print("static const airtime::HamNet kNets[] = {")
    print("  // name            kHz   mode  days  start(UTC min)  duration")
    print("\n".join(out))
    print("};")
    print("static const size_t kNetCount = sizeof(kNets) / sizeof(kNets[0]);")
    if skipped:
        print(f"// {skipped} row(s) skipped — see stderr", file=sys.stderr)


if __name__ == "__main__":
    main()
