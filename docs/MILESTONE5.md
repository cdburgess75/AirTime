# Milestone 5 — Field acceptance

The bench is finished. Everything below needs the radio outdoors, on its own
battery, listening to the actual ionosphere.

This is the only milestone that can fail in a way the simulator cannot predict,
because it is the only one that tests the whole chain at once — receiver, marker
detector, arbiter, NTP, the laptop's client — against a population of strangers
who have no idea they are being used as a time standard.

---

## The one number

**Median DT in WSJT-X.** Every decode carries a DT: how far off the transmitting
station appeared to be, measured against *this* laptop's clock. Hundreds of
operators cannot all be wrong in the same direction, so their DT values straddle
zero when the clock is right — and pile up on one side by exactly the error when
it is not.

    tools/dt_report.py ~/Library/Application\ Support/WSJT-X/ALL.TXT

| median DT | verdict |
|---|---|
| within ±0.20 s | **pass** — the clock is not the limiting factor |
| ±0.20 to ±0.50 s | usable (FT8 tolerates ~1 s) but off target |
| beyond ±0.50 s | a real clock error — capture diagnostics before rebooting |

Two things the report shows that a glance at the DT column will not:

- **Spread is not error.** The middle-50% figure is *their* scatter plus
  propagation. It will not drop below roughly 0.2 s however good this clock
  gets, and a wide spread with a centred median is a pass.
- **Drift hides inside a good median.** A clock losing time reads early at the
  start of the evening and late at the end, and the two average to something
  respectable. The report prints first-quarter against last-quarter separately
  for exactly that reason.

---

## Before you leave the house

```sh
cd ~/ats-mini && git pull && \
  arduino-cli compile --profile esp32s3-ospi \
    --build-property compiler.cpp.extra_flags=-DAIRTIME . && \
  arduino-cli upload --profile esp32s3-ospi -p $(ls /dev/cu.usbmodem* | head -1) .
```

Then, still indoors, confirm three things — each of these has failed silently
before, and none of them is worth discovering in a field:

1. **The clock reaches sync.** Leave it 20–30 minutes. The panel should show a
   white time, not amber, and an uncertainty under ~100 ms.
2. **The AP serves.** Join `AirTime`, then `sntp 192.168.4.1`. Anything inside
   ±0.2 s is fine at this stage.
3. **The status page loads.** `http://192.168.4.1/` — this is what you will read
   in the dark instead of tethering a laptop for serial.

Take: the radio, a charged battery, the external antenna, the laptop, and a
phone (for the status page). No network of any kind is required, which is the
entire point.

---

## The run

**Cold-start it in the field, on battery.** Not resumed from a warm boot at
home — the acquisition path is part of what is being tested, and a warm boot
skips it. Power on and note the time.

| record | why |
|---|---|
| minutes from power-on to a white (synced) clock | the cold-start cost, unmeasured outdoors so far |
| which WWV band produced the first marker | the band list is daytime-tuned; a night start may have to fail its way down |
| `sntp 192.168.4.1` a few times across the evening | independent of WSJT-X, and it carries the device's own claimed uncertainty |
| a photo of the status page late in the session | the band table, station biases and marker counts are the whole diagnosis |

Then point WSJT-X at 20 m or 40 m and let it run. **An hour of decodes is
enough; a full evening is better** — it is the multi-hour view that shows drift.

### Worth doing if you have the patience

Run the first half-hour on the laptop's own clock, then switch it to the radio
and run another half-hour:

    tools/dt_report.py ALL.TXT --split 2026-07-27T23:15

That prints both medians and the change between them, which is a far stronger
result than one number in isolation — it demonstrates the radio *doing* the job
rather than merely coinciding with a laptop that was already right.

---

## If it fails

Capture **before** power-cycling. Neither of these survives a reboot:

- the status page (screenshot the whole thing), and
- the serial log's `fix[...]` and `mkr[...]` lines if a laptop is to hand.

Then read them in this order, which is the order the failures actually happened
in during development:

| symptom | look at | what it means |
|---|---|---|
| DT offset near-constant all evening | status page → *correction still to apply* | a correction was accepted but is still slewing in. Non-zero here is a known error the device is already carrying. |
| DT offset a few hundred ms | status page → *FM stations, how late each one runs* | one biased station is dragging phase. It is meant to be learned and subtracted; if the bias table is empty, WWV never got fresh enough to teach it. |
| clock never goes white | status page → *WWV bands* | all-zero markers on every band means nothing was heard. Check the antenna, then the hour — 15 MHz is dead at night. |
| markers detected, clock unchanged | *Last word from each source* → `REJECTED` | the arbiter refused it. That is a designed behaviour with a reason, and the line says which. |
| DT drifts across the evening | *crystal* residual ppm | drift learning has not converged, or the temperature swing outran it. This is exactly what the multi-day soak is for. |

A **rejected** fix is not automatically a bug — the arbiter refuses corrections
it cannot corroborate, on purpose. What matters is whether it kept refusing.

---

## Definition of done

- [ ] Battery-only cold start in the field, on the external antenna, no infrastructure
- [ ] An evening of decodes with **median DT within ±0.2 s**
- [ ] Multi-day soak: drift learning holds across a temperature swing
- [ ] Numbers recorded in `STATUS.md`, including the ones that disappointed

That last one is not ceremony. Every useful thing in this repo came from a log
that did not match what the code claimed, and the failures were more productive
than the passes.
