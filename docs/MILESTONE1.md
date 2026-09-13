# Milestone 1 Runbook — the FM station survey

**Goal:** for every receivable FM station, learn three things: its PI code,
whether it transmits clock-time (RDS group 4A), and **its CT offset from
truth**. The offset is the number the arbiter's phase behaviour hinges on
(STATUS.md, "the caveat") — a station that is systematically 200 ms late
cannot be voted or weighted away, so we measure before we tune anything.

How the measurement works: the survey firmware prints each clock-time group
the instant it leaves the radio, `tools/survey_log.py` stamps that line
against the **Mac's NTP-synced clock**, and `tools/survey_report.py`
subtracts the minute the group asserts. No true clock is needed on the
device.

---

## 0. One-time: know where your clone is

The Milestone 0 sessions left a checkout on this Mac whose location has bitten
us once already (a stock build went onto the device because `cd
~/Documents/AirTime` failed, the `git pull` behind the `&&` never ran, and the
compile picked up the old tree in the current directory). Two rules from that:

- **`git remote -v` before you build.** It must say `cdburgess75/airtime`.
  If it errors or names another repo, you are in the wrong directory.
- **A successful flash is not a successful AirTime flash.** Check the compile
  output: an AirTime build lists `airtime_core` and `airtime_esp32` among the
  used libraries and is ~17 KB larger than stock (≈1,664 KB vs ≈1,647 KB).
  If those libraries are absent, you flashed stock, whatever flags you passed.

Cleanest fix if in doubt — fresh clone at a boring, non-iCloud path:

```
cd ~
git clone https://github.com/cdburgess75/airtime AirTime
```

## 1. Prepare the Mac clock

System Settings → General → Date & Time → **"Set time and date
automatically" ON**, internet connected, and leave it that way for the whole
run. macOS keeps itself within a few tens of ms of true time; offsets the
report shows under ~100 ms are measurement noise, not station bias.

## 2. Build and flash the survey firmware

Plug in the radio. One command at a time (paste-with-comments has burned us
before — see MILESTONE0 §4):

```
cd ~/AirTime/firmware/ats-mini
```
```
git pull
```
```
PORT=$(ls /dev/cu.usbmodem*)
```
```
arduino-cli compile --clean -e --build-property "compiler.cpp.extra_flags=-DAIRTIME_RDS_SURVEY" -p "$PORT" -u ats-mini
```

(`cu.*`, never `tty.*`. If several usbmodem devices exist, pick the radio's.)

## 3. Run the survey

Extend the telescopic antenna, then:

```
cd ~/AirTime
```
```
python3 tools/survey_log.py "$PORT" survey.log
```

You should see `SVY start`, a ~30 s scan pass (`SVY sig` per usable station,
then `SVY scan_done n=...`), then the dwell rotation: `SVY tune`, `SVY pi`,
`SVY ps`, and — the payload — `SVY ct` lines. Stations with no RDS drop out
after 12 s; stations with RDS but no clock-time show `SVY noct` after 75 s.

**Let it run at least 30 minutes** (an hour is better). Each `SVY round N
done` adds one CT sample per transmitting station, and the report uses the
median, so more rounds = a number you can trust. The display will look stale
or odd while this runs — the survey owns the radio; ignore the screen.

Stop with Ctrl-C.

## 4. The report

```
python3 tools/survey_report.py survey.log
```

Paste the whole output back into the session. It ends with a ready-made
`kFmStations` block, best-behaved stations first, with anything biased near
the ±250 ms RDS error budget flagged. Two follow-ups happen with that data:
the station list goes into `AirTimeMode.cpp`, and the measured offsets decide
whether the biased-station caveat in STATUS.md stays theoretical or gets the
principled fix (floor the posterior at the station's own accuracy).

## 5. Back to the real firmware

The survey build is a probe, not the product. When done:

```
cd ~/AirTime/firmware/ats-mini
```
```
arduino-cli compile --clean -e --build-property "compiler.cpp.extra_flags=-DAIRTIME" -p "$PORT" -u ats-mini
```

Confirm `airtime_core` / `airtime_esp32` appear in the used-libraries list,
then watch it live:

```
python3 -m serial.tools.miniterm "$PORT" 115200 --exit-char 3
```

Expect `AirTime: up.` at boot and a status pair every 10 s. Until the station
list is filled in it will honestly report itself unsynced — WWV listen
windows (audible!) and the "AirTime" access point still demonstrate the whole
path end to end.
