# Milestone 0 — Safety Net & Hardware Verification (Runbook)

**Purpose: make the ATS Mini un-brickable *before* anything of ours is flashed to it.**

This milestone exists because the owner has bricked a device before. Work through it
in order. Nothing here is exploratory — every step is either a backup, a rehearsal of
recovery, or a measurement that retires an assumption.

> **Status (2026-07-25): §1–§4 complete. The recovery drill has PASSED.** The device was
> erased and restored deliberately, and boots to stock. A verified 16 MB backup exists
> both in this repo and on the owner's machine. **§5 (stock `ats-mini` build) and §6
> (IO11 verification) remain.** Outcomes are recorded in [§7](#7-record-the-results).

---

## 0. Before you start

### The one fact that makes this safe

**The ESP32-S3 ROM bootloader lives in mask ROM. Flashing cannot destroy it.**
It is physically part of the silicon and is not writable by any flash operation. No
matter how badly a write goes, the chip can always be put back into download mode and
re-flashed. "Bricked" on an ESP32-S3 means *"I don't yet know the recovery incantation"*,
not *"the hardware is dead."* Section 4 is where you learn the incantation, on purpose,
while everything is still working.

### What you need

| | |
|---|---|
| Hardware | ATS Mini V4, USB-C data cable (**not** charge-only), charged battery |
| Software | Python 3 and `esptool` ≥ 4.0 (PlatformIO is not needed until §5) |
| Time | ~45 min, unhurried |
| Optional | HF antenna on the SMA port (needed for §6; evening is best) |

Install instructions are per-platform below. Verify with:

```sh
python3 -m esptool version
```

> **This runbook invokes esptool as `python3 -m esptool`** throughout. A `--user`
> install often puts the console script somewhere not on `PATH`, and the module form
> sidesteps that entirely. If plain `esptool` works for you, either is fine.

> **esptool v5** renamed commands to hyphenated forms (`read-flash`). The underscore
> forms below (`read_flash`) still work as aliases in v5 and are the only form in v4.

### Find the port

**macOS**

```sh
ls /dev/cu.*
```

The ESP32-S3 has native USB, so look for `/dev/cu.usbmodem…` (a board with a
USB-serial bridge instead shows as `cu.SLAB_USBtoUART` or `cu.wchusbserial…`).

> **Use `cu.*`, never `tty.*`.** Both appear for the same device. Opening a
> `tty.*` device blocks waiting for carrier detect, which looks exactly like a
> dead board. This is the most common way to waste an hour on a Mac.

```sh
export PORT=/dev/cu.usbmodem101
```

macOS does not gate serial access by group — no `dialout`, nothing to add
yourself to. If the port simply isn't there, it's the cable or the driver
(see [§8](#8-if-something-goes-wrong)).

**Installing esptool on macOS.** No Homebrew required. Check for Python first:

```sh
python3 --version
```

- **Prints a version** → install into your user directory (no admin, no
  virtualenv, nothing system-wide). **Upgrade pip first** — the Command Line
  Tools ship pip 21.2.4, which is too old to fetch the prebuilt `cryptography`
  wheel esptool depends on; without this it tries to compile it from source,
  needs a Rust toolchain, and fails with
  `Could not build wheels for cryptography`:
  ```sh
  python3 -m pip install --user --upgrade pip
  python3 -m pip install --user esptool
  export PATH="$PATH:$(python3 -c 'import site; print(site.USER_BASE)')/bin"
  esptool version
  ```
  Add that `export` line to `~/.zshrc` to make it stick.

  > **If `esptool` is still "command not found"** after a successful install, it
  > is only a PATH problem — the package is there. Invoke the module directly
  > instead, which needs no PATH at all:
  > ```sh
  > python3 -m esptool version
  > ```
  > **Every `esptool …` command in this runbook can be written
  > `python3 -m esptool …`.** Depending on the version the installed script may
  > also be named `esptool.py` rather than `esptool`; check with
  > `ls "$(python3 -c 'import site; print(site.USER_BASE)')/bin"`.

- **Command not found**, or it opens a dialog → install Apple's Command Line
  Tools (gives you `python3` and `git`), then retry the above:
  ```sh
  xcode-select --install
  ```

- **If Homebrew's Python is what you have**, `pip install` may refuse with
  `externally-managed-environment`. Use `pipx install esptool`, or add
  `--break-system-packages` to the `--user` install above.

A standalone prebuilt binary is also published on Espressif's esptool GitHub
releases page (pick the macOS asset matching your CPU — arm64 for Apple Silicon,
amd64 for Intel). If you go that route, macOS quarantines downloaded binaries;
clear it with `xattr -d com.apple.quarantine ./esptool` or the first run will be
blocked.

**Linux**

```sh
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
export PORT=/dev/ttyACM0
```

Permission denied? Add yourself to the serial group, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"
```

### Rules for this session

> ### ⚠️ Paste one command at a time in §4
>
> **Interactive zsh (the macOS default) does not accept `#` comments**, and it expands
> `~4` as a *directory stack* reference. A comment containing something like "~4 min"
> raises `not enough directory stack entries`, and that expansion error **flushes the
> rest of the pasted buffer**.
>
> This happened during a real run of §4: a two-command paste executed the **erase** and
> silently dropped the **restore**, leaving the device blank with no follow-up command.
> Recoverable — that is the whole point of §4 — but do not rely on it.
>
> In §4, paste and run each command **individually**, and confirm each one's output
> before the next. The code blocks below deliberately contain no comments and no
> line continuations. (`setopt interactivecomments` fixes the comment half if you
> want it, but one-at-a-time is the rule that actually protects you.)

- **Never** interrupt a `write_flash` mid-run. Reads are always safe; writes are not.
- Keep the battery charged and the cable seated. A brownout during a write is the one
  way to make this genuinely annoying.
- Do §1–§4 in one sitting. A backup you have not *tested restoring* is not a backup.

---

## 1. Identify the chip and its partition layout

```sh
python3 -m esptool --chip esp32s3 --port "$PORT" flash_id
```

Expected: `ESP32-S3`, flash size **16MB**. Record what you actually see.

If this command fails, do not proceed — get communication working first
(see [§8 Troubleshooting](#8-if-something-goes-wrong)).

Now read the partition table, which tells you whether anything lives above 2 MB:

```sh
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash 0x8000 0xc00 partitions.bin

python "$IDF_PATH/components/partition_table/gen_esp32part.py" partitions.bin
```

Note the highest offset+size any partition reaches. **If anything extends past
0x200000, a 2 MB backup would not fully restore this device** — which is exactly why
§2 takes the whole chip.

---

## 2. Back up the stock firmware

**Take the full 16 MB.** Measured on this unit: the `settings` partition at
**0x7e0000 is 28.7% populated** — real per-unit configuration living four times past
the 2 MB mark. PLAN.md §7's `0x0`+`0x200000` read cannot capture it, and an
`erase_flash` followed by a 2 MB restore would wipe it permanently.

> **Correction to an earlier version of this document.** It claimed a 2 MB read would
> stop *inside* `app0` and truncate the application. Occupancy measurement refutes
> that: `app0`'s partition spans 3 MB but holds only ~1.5 MB of image, which fits
> below 0x200000 — the application would have survived. `settings` is the partition
> actually lost. The conclusion (full-chip backup) is unchanged; the reason is not.
> Recorded because a safety document that argues from an assumed layout instead of a
> measured one is the kind of thing that gets trusted and then fails.

Nothing on this unit lives above 0x800000 (the top 8 MB reads 0.0% used), so an 8 MB
image would in fact suffice *today*. Full-chip is still the instruction: it costs a
couple of extra minutes and does not depend on that staying true — notably after §5
flashes a build whose partition table may differ.

> **Do not take a partial image as a "fast restore" option.** One image, full chip,
> no ambiguity. A file that looks like a backup but silently omits a populated
> partition is worse than no backup, because it is what gets reached for during a
> failure.

```sh
mkdir -p ~/airtime-backup && cd ~/airtime-backup

python3 -m esptool --chip esp32s3 --port "$PORT" --baud 921600 \
        read_flash 0x0 0x1000000 stock-full-16mb.bin
```

Measured on the owner's unit: **227.8 s (~4 min) at 589 kbit/s** with `--baud 921600`.
If the read errors out, drop to `--baud 460800`. A read is safe to interrupt and
re-run — the never-interrupt rule applies to `write_flash` in §4c.

### Checksum it, and commit the checksum

macOS has `shasum`, not `sha256sum`:

macOS:

```sh
shasum -a 256 stock-full-16mb.bin | tee SHA256SUMS
```

Linux:

```sh
sha256sum stock-full-16mb.bin | tee SHA256SUMS
```

**Where the `.bin` itself goes — owner decision (2026-07-25): committed to this repo.**
See [`STATUS.md`](STATUS.md#setup-decisions) for the storage rationale and the
copyright caveat that applies if the repository is ever made public.

`SHA256SUMS` belongs in the repo regardless, so any future copy can be verified.

---

## 3. Verify the backup before trusting it

A truncated or all-`0xFF` image looks like a file and restores like a disaster.

```sh
cd ~/airtime-backup
ls -l stock-full-16mb.bin
shasum -a 256 -c SHA256SUMS
```

**If you have this repo checked out**, one command does the rest — it decodes the
partition table out of the image, checks every header, reports what each partition
actually holds, and fails loudly on a truncated image:

```sh
python3 tools/inspect_flash.py ~/airtime-backup/stock-full-16mb.bin
```

Otherwise, the equivalent inline (Python 3.9 safe — no backslashes inside f-strings):

```sh
python3 - <<'PY'
d = open('stock-full-16mb.bin','rb').read()
print('size        :', len(d), '(expect 16777216)')
print('0xFF bytes  : {:.1%}'.format(d.count(b'\xff')/len(d)))
print('bootloader  :', hex(d[0]), '(0xe9 = valid ESP image header)')
print('ptable magic:', hex(d[0x8000]), hex(d[0x8001]), '(expect 0xaa 0x50)')
# app0 lives at 0x10000 and must also start with an image header
print('app0 header :', hex(d[0x10000]), '(0xe9)')
# the region the 2 MB read would have missed entirely
print('littlefs@0x610000 non-erased bytes: {:.1%}'.format(
      1 - d[0x610000:0x7e0000].count(b'\xff')/(0x7e0000-0x610000)))
PY
```

Expect `0xe9` for both headers and `0xaa 0x50` at the partition table. A high overall
0xFF fraction is normal — half the chip is unpartitioned.

> **On the owner's unit `littlefs` reads 0.0% — entirely erased.** That is a property
> of the device (the partition is declared but never formatted), not a bad read. Do
> not treat an empty littlefs as a failed backup. The checks that actually prove the
> image is good are the three magic bytes and the exact 16777216-byte size.

```sh
cd ~
```

---

## 4. The recovery drill — **the point of this milestone**

You are about to erase the device deliberately, while calm, with a verified backup in
hand. Do this now so that the first time you erase this chip is *not* the day something
has gone wrong.

### 4a. Learn to force download mode (do this first, dry)

esptool normally auto-resets the board into download mode over DTR/RTS — that's why §1
worked. The manual method is your fallback for when auto-reset fails:

1. Hold the **BOOT** button (locate it on your unit — it may require opening the case;
   record where it is in §7).
2. While holding BOOT, power-cycle: tap **RESET**, or unplug/replug USB.
3. Release BOOT.
4. Confirm the chip is listening: `python3 -m esptool --chip esp32s3 --port "$PORT" chip_id`

Practice this **before** the erase. If your unit has no reachable BOOT button, note that
in §7 and rely on auto-reset — it works fine; you just have one fewer fallback.

> **Have the §4c restore command on screen before you run §4b.** Once the erase
> completes the device is dark, and you do not want to be composing the next command
> then. Paste these one at a time.

### 4b. Erase

```sh
python3 -m esptool --chip esp32s3 --port "$PORT" erase_flash
```

Expect `Chip erase completed successfully` in a few seconds. The device is now blank and
will not boot. **This is expected.** Confirm it: the screen stays dark or the radio does
nothing. Sit with it for a second — this is the state you were afraid of, and you are
about to walk straight back out of it.

### 4c. Restore

Run this as a **single line**, on its own:

```sh
python3 -m esptool --chip esp32s3 --port "$PORT" --baud 921600 write_flash 0x0 stock-full-16mb.bin
```

Run it from the directory holding the image (`~/airtime-backup`). Two to four minutes —
the mostly-erased upper half compresses away, so the write is faster than the read.
**Do not interrupt.**

If `$PORT` has been lost from the shell, set it again first, on its own line:

```sh
export PORT=/dev/cu.usbmodem14401
```

### 4d. Confirm

Power-cycle. The radio should boot to stock firmware exactly as it did before: same
screen, tuning works, audio works.

**Prove it rigorously**, rather than trusting that it looks right — read the flash back
and compare it against the backup:

```sh
cd ~/airtime-backup
python3 -m esptool --chip esp32s3 --port "$PORT" --baud 921600 \
        read_flash 0x0 0x1000000 after-restore-16mb.bin

python3 <repo>/tools/inspect_flash.py stock-full-16mb.bin after-restore-16mb.bin
```

`nvs`, `otadata` and `coredump` are expected to differ — the device writes them as it
boots. **`app0`, `app1`, `settings` and the bootloader must match exactly**; the tool
flags it if they don't.

**You have now bricked and un-bricked this device on purpose.** Everything after this is
recoverable by repeating §4c.

> If the restore does *not* boot: repeat §4a to force download mode, then re-run §4c.
> Still nothing? See [§8](#8-if-something-goes-wrong). The ROM loader is intact — it
> cannot not be.

---

## 5. Build and flash stock `ats-mini`

Prove the toolchain end-to-end with *unmodified* upstream code, so that any later
problem is unambiguously ours and not the build.

### 5a. Bring the firmware base into the repo

Per the project decision, upstream is vendored with **git subtree** — one clone, no
submodule ceremony, and a real upstream merge path later:

```sh
git subtree add --prefix=firmware/ats-mini \
    https://github.com/esp32-si4732/ats-mini main --squash
```

Future upstream updates, when you want them:

```sh
git subtree pull --prefix=firmware/ats-mini \
    https://github.com/esp32-si4732/ats-mini main --squash
```

### 5b. Pick the PSRAM variant — this matters

The upstream project ships **OSPI** and **QSPI** builds. The wrong one boots but reports
**zero PSRAM**. Check the envs available:

```sh
cd firmware/ats-mini
grep -E '^\[env' platformio.ini
```

Build and flash one variant, unmodified:

```sh
pio run -e <variant> -t upload --upload-port "$PORT"
```

### 5c. Verify

On the device: **Settings → About**. **PSRAM must be non-zero.** If it reads zero, flash
the other variant and re-check. Record the winner in §7 — every future build uses it.

Confirm normal radio operation: FM tunes, HF tunes, audio out of the speaker.

---

## 6. IO11 verification — retires the standing assumption

**The assumption:** IO11 (ADC2_CH0) carries speaker audio on this unit. V4a/V4b route it
at the factory; the original V4 has solder pads only. Everything in Milestone 3 depends
on this, so measure it rather than hope.

**The method** (from HJBerndt's documentation): feed the receiver a steady 1 kHz tone and
watch whether the firmware's ADC-driven backlight responds to it.

1. **Antenna on**, HF connected. Evening gives the best 10 MHz propagation.
2. Flash the **HJBerndt binary** (closed-source; it is a *verification tool* here, not
   donor code — see PLAN.md §6). Keep your §2 backup handy; you'll reflash after.
3. Tune **9999.000 kHz USB**. WWV's 10 MHz carrier beats against your BFO to a continuous
   **1000 Hz** tone. *(Alternative: 4999.000 kHz USB against WWV 5 MHz.)*
4. Set **volume ≈ 35** — the tap is fed from the amplifier output, so it needs real
   audio level.
5. Enter **Decoder → Tune/BL**.

**Read the result:**

| Observation | Meaning | Action |
|---|---|---|
| Backlight **flickers** in time with the tone | **Tap confirmed.** IO11 carries audio | Assumption retired ✅ Proceed |
| **No flicker at any volume** | Original V4 — pads only, not routed | One jumper wire: **amp IC pin 8 → IO11**, then retest |
| No tone audible at all | Propagation/antenna/tuning issue | Fix reception first; this is not an IO11 result |

The third row matters: **a null result only counts if you can actually hear the tone.**
Don't conclude "no tap" from a quiet band.

6. **Reflash** stock or your `ats-mini` build afterwards (§4c or §5b).

---

## 7. Record the results

Fill this in and commit it. Future sessions read this table instead of guessing.

### §1 identification — recorded 2026-07-25

Raw `flash_id` output from the owner's unit:

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: 20:6e:f1:b5:90:30
Manufacturer: 46   Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

Decoded partition table:

```
label            type  subtype     offset      size       end
nvs              data  0x2         0x9000    0x5000    0xe000
otadata          data  0x0         0xe000    0x2000   0x10000
app0             app   0x10       0x10000  0x300000  0x310000
app1             app   0x11      0x310000  0x300000  0x610000
littlefs         data  0x83      0x610000  0x1d0000  0x7e0000
settings         data  0x2       0x7e0000   0x10000  0x7f0000
coredump         data  0x3       0x7f0000   0x10000  0x800000

highest end offset: 0x800000 (8.00 MB)
```

Per-partition occupancy of the backup image (fraction of non-erased bytes):

```
partition     used  first bytes
nvs          52.3%  feffffff05000000     real NVS data
otadata       0.1%  01000000ffffffff     OTA select entry -> boots app0
app0         49.6%  e906023f8c613740     0xe9 image header; ~1.5 MB of firmware
app1          0.0%  ffffffffffffffff     empty — no second OTA slot in use
littlefs      0.0%  01000000f00ffff7     formatted (superblock) but no files
settings     28.7%  fcffffff00000000     real per-unit configuration
coredump      0.0%  ffffffffffffffff     no crashes recorded

unpartitioned above 0x800000: 0.0% used
```

Conclusions worth carrying forward:

- **`settings` (0x7e0000, 28.7% used) is why the full backup matters.** It is real
  per-unit configuration sitting far past the 2 MB mark. `app0`'s content, contrary
  to an earlier assumption here, fits under 2 MB — see the correction in §2.
- **`app0` is a 3 MB partition holding ~1.5 MB today**, so AirTime has a comfortable
  firmware size budget.
- **`app1` is empty and `otadata` selects app0** — a single active image, with the
  second OTA slot free.
- **A different partition table from the `ats-mini` fork would orphan stock
  `settings`.** Recoverable from the §2 image, which is precisely why §2 comes first.

- **8 MB PSRAM ⇒ the `R8` part ⇒ octal PSRAM ⇒ try the OSPI build first** (§5b).
  Confirm the usual way — non-zero PSRAM in Settings→About — but this should save
  a flash/check round trip.
- **USB mode is USB-Serial/JTAG and esptool auto-reset worked** with no button
  held, so download mode is reachable automatically. The manual BOOT method (§4a)
  is a fallback here, not a prerequisite. This also means the ROM's USB CDC keeps
  enumerating after an erase, which is exactly what makes §4 safe.

### Results table

| Item | Result | Notes |
|---|---|---|
| Date run | 2026-07-25 (§1 only) | §2 onward not yet run |
| Chip / flash size reported | **ESP32-S3 (QFN56) rev v0.2 / 16MB** ✅ | as expected; quad flash, 3.3 V |
| PSRAM detected | **8 MB (AP_3v3)** ✅ | ⇒ OSPI build variant expected |
| USB mode | **USB-Serial/JTAG** | auto-reset into download mode works |
| MAC | `20:6e:f1:b5:90:30` | unit identity; also predicts the SoftAP BSSID |
| Highest partition offset | **0x800000 (8 MB)** ✅ | nothing above it; `settings` at 0x7e0000 is 28.7% populated ⇒ **2 MB backup would lose it** |
| **Full backup SHA-256** | `aeb512fea414b0ecb077c1564ca5298ac18a0e960c2b7342ac29c415a384db89` ✅ | 16777216 bytes, verified 2026-07-25; read in 227.8 s @ 921600 |
| Backup stored where | **`firmware/backup/stock-full-16mb.bin` in this repo** ✅ + owner's Mac | committed 2026-07-25; checksum re-verified against the recorded SHA-256 after push, and `tools/inspect_flash.py` re-validated the stored copy structurally |
| BOOT button location | not located / not needed | auto-reset worked every time; case never opened |
| Forced download mode | **automatic** ✅ | esptool DTR/RTS auto-reset over USB-Serial/JTAG; manual BOOT method never required |
| **Recovery drill (§4)** | ✅ **PASSED 2026-07-25** | `erase_flash` (3.1 s) → full 16 MB `write_flash` → boots to stock, confirmed by the owner |
| PSRAM variant | | OSPI / QSPI — the one showing non-zero PSRAM |
| PSRAM reported in About | | must be non-zero |
| Stock radio operation | ⬜ confirmed | FM + HF tune, audio out |
| **IO11 tap (§6)** | ⬜ confirmed / ⬜ jumpered | flicker seen? at what volume? |
| V4 sub-revision concluded | | V4 (pads) or V4a (routed) |

**Milestone 0 is green when the recovery drill passed and the IO11 row is resolved**
(either confirmed as routed, or jumpered and then confirmed).

---

## 8. If something goes wrong

**esptool can't see the device**
- Cable is charge-only → swap it. This is the single most common cause.
- Wrong port → re-run the listing with the device unplugged, then plugged, and
  diff the two. On macOS make sure you picked `cu.*`, not `tty.*`.
- Permissions (Linux only) → `dialout` group (see §0), and log out/in.
- Something else holds the port → close serial monitors, PlatformIO, Arduino IDE.
- Force download mode manually (§4a), then retry.

**"Failed to connect: No serial data received"**
- Almost always download mode. Hold BOOT, power-cycle, release, retry.

**Write fails partway through**
- Do not panic and do not unplug. Re-run the same `write_flash`. Lower to
  `--baud 460800`. The ROM loader is still there; a partial write is just an incomplete
  file, not damage.

**Device won't boot after restore**
- Force download mode (§4a) → `erase_flash` → re-run §4c.
- Verify the image first: `sha256sum -c SHA256SUMS`.

**PSRAM reads zero in About**
- Wrong build variant. Flash the other one (§5b).

**No 1 kHz beat in §6**
- Check antenna and band conditions; try 4999.000 kHz USB against WWV 5 MHz, or wait for
  evening. Confirm you can *hear* the tone before drawing any IO11 conclusion.

---

## What comes after

With §4 passed and §6 resolved, the pre-flash gate is lifted and the
[host-tested core](../lib/airtime_core/README.md) starts meeting hardware — the thin
adapters of [`ARCHITECTURE.md`](ARCHITECTURE.md): `RdsSource`, `Sampler`,
`MonotonicClock`, `DriftStore`/`TimeStore`, `NtpResponder`, `WiFiControl`, `Ui`.

The **calibration constant** (PLAN.md §4) is measured once the `Sampler` adapter runs —
it is the one number the host tests cannot know.
