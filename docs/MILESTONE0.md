# Milestone 0 — Safety Net & Hardware Verification (Runbook)

**Purpose: make the ATS Mini un-brickable *before* anything of ours is flashed to it.**

This milestone exists because the owner has bricked a device before. Work through it
in order. Nothing here is exploratory — every step is either a backup, a rehearsal of
recovery, or a measurement that retires an assumption.

> **Status: not yet run.** Record outcomes in [§7](#7-record-the-results) as you go and
> commit the file — later sessions read it to know what the hardware actually is.

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
| Software | `esptool` ≥ 4.0, PlatformIO, Python 3 |
| Time | ~45 min, unhurried |
| Optional | HF antenna on the SMA port (needed for §6; evening is best) |

```sh
pip install --upgrade esptool
esptool version
```

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
export PORT=/dev/cu.usbmodem101      # substitute what you actually saw
```

macOS does not gate serial access by group — no `dialout`, nothing to add
yourself to. If the port simply isn't there, it's the cable or the driver
(see [§8](#8-if-something-goes-wrong)).

Installing esptool on macOS: `brew install esptool` is the least friction.
(`pip3 install esptool` also works, but Homebrew's Python will refuse with
`externally-managed-environment` unless you use `pipx install esptool`.)

**Linux**

```sh
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
export PORT=/dev/ttyACM0
```

Permission denied? Add yourself to the serial group, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"     # 'uucp' on Arch
```

### Rules for this session

- **Never** interrupt a `write_flash` mid-run. Reads are always safe; writes are not.
- Keep the battery charged and the cable seated. A brownout during a write is the one
  way to make this genuinely annoying.
- Do §1–§4 in one sitting. A backup you have not *tested restoring* is not a backup.

---

## 1. Identify the chip and its partition layout

```sh
esptool --chip esp32s3 --port "$PORT" flash_id
```

Expected: `ESP32-S3`, flash size **16MB**. Record what you actually see.

If this command fails, do not proceed — get communication working first
(see [§8 Troubleshooting](#8-if-something-goes-wrong)).

Now read the partition table, which tells you whether anything lives above 2 MB:

```sh
esptool --chip esp32s3 --port "$PORT" read_flash 0x8000 0xc00 partitions.bin

# Decode it (ships with ESP-IDF; skip if not installed — §2 backs up everything anyway)
python "$IDF_PATH/components/partition_table/gen_esp32part.py" partitions.bin
```

Note the highest offset+size any partition reaches. **If anything extends past
0x200000, a 2 MB backup would not fully restore this device** — which is exactly why
§2 takes the whole chip.

---

## 2. Back up the stock firmware

**Take the full 16 MB.** The plan's `0x0`/`0x200000` read covers the app region, but this
is a 16 MB part and stock firmware may keep SPIFFS/NVS/calibration data higher up.
Three extra minutes removes all doubt.

```sh
mkdir -p firmware/backup && cd firmware/backup

esptool --chip esp32s3 --port "$PORT" --baud 921600 \
        read_flash 0x0 0x1000000 stock-full-16mb.bin
```

Takes ~3 min at 921600 baud. If the read errors out, drop to `--baud 460800`.

Also take the plan's 2 MB app-region image as a fast-restore option:

```sh
esptool --chip esp32s3 --port "$PORT" --baud 921600 \
        read_flash 0x0 0x200000 stock-app-2mb.bin
```

### Checksum both, and commit the checksums

```sh
sha256sum stock-full-16mb.bin stock-app-2mb.bin | tee SHA256SUMS
cd ../..
```

**Where the `.bin` itself goes** depends on whether this repo is public:

- **Private repo** → commit the `.bin` files. One-time 18 MB, always cloned with the
  repo, offsite and versioned. Best availability when you actually need it.
- **Public repo** → **do not commit them.** Stock firmware is AMNVOLT's copyrighted
  binary. Keep the images in local + cloud backup and commit only `SHA256SUMS`.

Either way `SHA256SUMS` belongs in the repo, so any future copy can be verified.

---

## 3. Verify the backup before trusting it

A truncated or all-`0xFF` image looks like a file and restores like a disaster.

```sh
cd firmware/backup
ls -l stock-full-16mb.bin                       # expect exactly 16777216 bytes
sha256sum -c SHA256SUMS                         # expect: OK

# Sanity: the image should NOT be one giant run of erased flash
python3 - <<'PY'
d = open('stock-full-16mb.bin','rb').read()
print('size      :', len(d))
print('0xFF bytes: {:.1%}'.format(d.count(b'\xff')/len(d)))
print('magic 0xE9:', hex(d[0]), '(0xe9 = valid ESP image header)')
PY
```

Expect `magic 0xE9` and a 0xFF fraction well under 100% (a 16 MB image of a small app is
*mostly* erased — that's fine and normal; what you're ruling out is 100%).

```sh
esptool --chip esp32s3 image_info stock-app-2mb.bin   # should parse as an ESP32-S3 image
cd ../..
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
4. Confirm the chip is listening: `esptool --chip esp32s3 --port "$PORT" chip_id`

Practice this **before** the erase. If your unit has no reachable BOOT button, note that
in §7 and rely on auto-reset — it works fine; you just have one fewer fallback.

### 4b. Erase

```sh
esptool --chip esp32s3 --port "$PORT" erase_flash
```

The device is now blank and will not boot. **This is expected.** Confirm it: the screen
stays dark or the radio does nothing. Sit with it for a second — this is the state you
were afraid of, and you are about to walk straight back out of it.

### 4c. Restore

```sh
esptool --chip esp32s3 --port "$PORT" --baud 921600 \
        write_flash 0x0 firmware/backup/stock-full-16mb.bin
```

~4 min. **Do not interrupt.**

### 4d. Confirm

Power-cycle. The radio should boot to stock firmware exactly as it did before: same
screen, tuning works, audio works.

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

| Item | Result | Notes |
|---|---|---|
| Date run | | |
| Chip / flash size reported | | expect ESP32-S3 / 16MB |
| Highest partition offset | | does anything live above 0x200000? |
| Full backup SHA-256 | | from `firmware/backup/SHA256SUMS` |
| Backup stored where | | in-repo (private) / external (public) |
| BOOT button location | | accessible without opening case? |
| Forced download mode | ⬜ rehearsed | method that worked |
| **Recovery drill (§4)** | ⬜ **passed** | erased and restored successfully |
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
