# Pointing a computer at AirTime

**WSJT-X and JS8Call have no NTP client.** They read the operating system's
clock and nothing else — there is no time-source setting to find, and no
plugin that adds one. So the whole job is: make the OS take its time from the
radio. WSJT-X then inherits it automatically.

Join the **AirTime** access point first. The radio is `192.168.4.1` and answers
NTP on the standard port. It has no internet connection and is not supposed to
— that is the entire point.

Check the radio is serving before configuring anything:

```
sntp 192.168.4.1
```

A reading like `-0.005106 +/- 0.070630` means the radio is disciplined and
answering. If the `+/-` figure is a second or more, the radio is telling you it
is not yet synchronised — wait for it rather than syncing to it. It reports
that honestly (unsynchronised responses are flagged in the packet, so a real
NTP client would refuse them; `sntp` prints them anyway).

## macOS

```
sudo systemsetup -setusingnetworktime off
sudo sntp -sS 192.168.4.1
```

The first command matters as much as the second: with automatic time enabled,
`timed` keeps trying to reach Apple's servers and will overwrite your setting
the moment it finds any route to the internet.

Re-run the `sntp -sS` line before each operating session. A laptop's own
crystal drifts on the order of tens of milliseconds per hour, so an hourly
repeat is generous for FT8's needs.

**When you return to the internet**, put it back or the machine free-runs
indefinitely:

```
sudo systemsetup -setusingnetworktime on
```

Continuous alternative — macOS disciplines itself from the radio, no repeating
by hand:

```
sudo systemsetup -setnetworktimeserver 192.168.4.1
sudo systemsetup -setusingnetworktime on
```

Tidier, but `timed` polls on its own schedule and can be slow to react, and the
server setting has to be pointed back at Apple afterwards. For a single evening
the manual step is simpler to reason about.

## Linux

With chrony (`/etc/chrony/chrony.conf`), alongside or instead of your usual
pool lines:

```
server 192.168.4.1 iburst prefer
```

Then `sudo systemctl restart chrony && chronyc sources`. With `ntpd`, the
equivalent `server 192.168.4.1 iburst prefer` in `/etc/ntp.conf`.

One-shot, no config file: `sudo chronyd -q 'server 192.168.4.1 iburst'`.

## Windows

As Administrator:

```
w32tm /config /manualpeerlist:"192.168.4.1,0x8" /syncfromflags:manual /update
w32tm /resync
```

`w32tm /query /status` shows what it settled on. Windows' built-in client is
coarse by default; if the DT column looks noisy, Meinberg NTP is the usual
remedy and takes `server 192.168.4.1 iburst prefer` like any ntpd.

## Verifying, and what the DT column is telling you

Once decoding, WSJT-X's **DT** column is the acceptance test (PLAN.md §7,
Milestone 5) — and it is also a second, independent measurement of the
receive-chain calibration constant, arriving from the opposite direction to an
`sntp` reading:

* **Scattered either side of 0.0** — synced. Nothing to do.
* **Clustered at a consistent non-zero value**, the same sign for every station
  on the band — that offset is *your* clock, not theirs. Feed it back into
  `AppConfig::wwv_calibration_us` in `firmware/ats-mini/ats-mini/AirTimeMode.cpp`
  (the constant is the receive-chain latency the radio cannot measure about
  itself; see PLAN.md §4).
* **Individual stations scattered by seconds** — that is them, not you. Common
  on any band.

FT8 tolerates roughly ±1 s and decodes best well inside that. A disciplined
AirTime serves to a few tens of milliseconds, so the margin is large; the DT
column should never be the thing that fails.
