#!/usr/bin/env python3
"""Turn a survey_log.py capture into the Milestone 1 station survey table.

Usage:
    python3 tools/survey_report.py survey_*.log

For each station: frequency, PI, PS name, signal, how many clock-time groups
were caught, and the measured CT offset from truth (median across samples,
plus spread). Offset convention: positive = the station's clock-time arrived
LATE relative to the minute it asserts.

The arithmetic per `SVY ct` line:

    offset = (line arrival wall time) - (group transmission time, 87.6 ms)
             - (asserted minute, UTC)

The 87.6 ms term is deterministic: an RDS group is 104 bits at 1187.5 bps and
the standard aligns the minute edge with the START of the 4A group, while the
radio can only hand it over after receiving all of it. What remains in the
number is station bias plus measurement noise (chip FIFO latency, device loop
and USB/serial delay — a few tens of ms, always positive). Rule of thumb:
|median| under ~100 ms is indistinguishable from zero here; a 220 ms-class
bias — the case the arbiter caveat in STATUS.md worries about — stands out
clearly. CT samples with any block-error digit above 1 are excluded.
"""

import re
import statistics
import sys

GROUP_TX_S = 104 / 1187.5  # 87.6 ms

RE_LINE = re.compile(r"^(\d+(?:\.\d+)?) (.*)$")
RE_SIG = re.compile(r"^SVY sig f=(\d+) rssi=(\d+) snr=(\d+)")
RE_PS = re.compile(r'^SVY ps f=(\d+) pi=([0-9A-F]{4}) ps="(.*)"')
RE_CT = re.compile(r"^SVY ct f=(\d+) pi=([0-9A-F]{4}) utc=(\d+) ble=(\d{4})")
RE_NOSYNC = re.compile(r"^SVY nosync f=(\d+)")
RE_NOCT = re.compile(r"^SVY noct f=(\d+) pi=([0-9A-F]{4})")


def mhz(f10k):
    return f10k / 100.0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    sig, ps, offsets, pi_of = {}, {}, {}, {}
    nosync, noct, rejected = set(), set(), 0

    for path in sys.argv[1:]:
        for raw in open(path):
            m = RE_LINE.match(raw.strip())
            if not m:
                continue
            wall, line = float(m.group(1)), m.group(2)

            if s := RE_SIG.match(line):
                sig[int(s.group(1))] = (int(s.group(2)), int(s.group(3)))
            elif s := RE_PS.match(line):
                ps[int(s.group(1))] = s.group(3).strip()
            elif s := RE_CT.match(line):
                f, pi, utc, ble = (int(s.group(1)), s.group(2),
                                   int(s.group(3)), s.group(4))
                if any(int(d) > 1 for d in ble):
                    rejected += 1
                    continue
                pi_of[f] = pi
                offsets.setdefault(f, []).append(wall - GROUP_TX_S - utc)
            elif s := RE_NOSYNC.match(line):
                nosync.add(int(s.group(1)))
            elif s := RE_NOCT.match(line):
                noct.add(int(s.group(1)))
                pi_of.setdefault(int(s.group(1)), s.group(2))

    if not (sig or offsets):
        sys.exit("no SVY lines found — is this a survey_log.py capture?")

    print(f"{'MHz':>6} {'PI':>5} {'PS':<9} {'rssi/snr':>8} "
          f"{'n(CT)':>5} {'median':>9} {'min..max':>17}")
    for f in sorted(sig | offsets.keys()):
        r, s = sig.get(f, (0, 0))
        name = ps.get(f, "")
        pi = pi_of.get(f, "----")
        if f in offsets:
            ms = sorted(x * 1000 for x in offsets[f])
            med = statistics.median(ms)
            print(f"{mhz(f):>6.1f}  {pi} {name:<9} {r:>3}/{s:<3} "
                  f"{len(ms):>5} {med:>+8.0f}ms {ms[0]:>+7.0f}..{ms[-1]:+.0f}ms")
        else:
            why = "no RDS sync" if f in nosync else \
                  "RDS, no CT" if f in noct else "no CT seen"
            print(f"{mhz(f):>6.1f}  {pi} {name:<9} {r:>3}/{s:<3} "
                  f"{0:>5} {'—':>10} ({why})")
    if rejected:
        s = "s" if rejected != 1 else ""
        print(f"\n({rejected} CT sample{s} excluded for block errors)")

    usable = {f: statistics.median(v) * 1000 for f, v in offsets.items()
              if len(v) >= 2}
    if usable:
        print("\nSuggested kFmStations for AirTimeMode.cpp"
              " (best phase behaviour first):")
        for f in sorted(usable, key=lambda f: abs(usable[f])):
            flag = "" if abs(usable[f]) < 250 else \
                "  // CAUTION: bias near/over RDS error budget"
            print(f"    {f},  // {mhz(f):.1f} MHz {ps.get(f, '')}"
                  f" pi={pi_of.get(f, '?')}"
                  f" offset {usable[f]:+.0f} ms (n={len(offsets[f])}){flag}")
        print("\nSingle-sample stations were omitted; run the survey longer"
              " to qualify them.")


if __name__ == "__main__":
    main()
