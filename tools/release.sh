#!/bin/bash
# Build the flashable images, so somebody can try AirTime without installing
# arduino-cli, an ESP32 core and four libraries.
#
#   tools/release.sh [airtime|fast]
#     -> dist/airtime-<version>-<flavor>.bin   everything, flash at 0x0 (first install)
#     -> dist/airtime-<version>-<flavor>-app.bin  the app only, flash at 0x10000 (update;
#                                                 keeps settings and everything learned)
#
# The first is a MERGED image: bootloader + partition table + application in
# one file at offset 0. That is what a web flasher (esptool-js, ESP Web Tools)
# expects, and it removes the commonest way a first flash goes wrong — three
# files at three offsets, one of them mistyped. It also erases the settings
# partition, which is right for a first install and wrong for an update; hence
# the second file.
#
# Flashing:
#   esptool --chip esp32s3 write-flash 0x0     dist/airtime-<version>-<flavor>.bin
#   esptool --chip esp32s3 write-flash 0x10000 dist/airtime-<version>-<flavor>-app.bin
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="$ROOT/firmware/ats-mini/ats-mini"
FLAVOR="${1:-airtime}"
case "$FLAVOR" in
  airtime) FLAGS="-DAIRTIME" ;;
  fast)    FLAGS="-DAIRTIME -DAIRTIME_FAST_LISTEN" ;;
  *) echo "usage: $0 [airtime|fast]" >&2; exit 2 ;;
esac

VER=$(sed -n 's/.*AIRTIME_VERSION "\([^"]*\)".*/\1/p' "$SKETCH/Common.h" | head -1)
[ -n "$VER" ] || { echo "release: cannot read AIRTIME_VERSION from Common.h" >&2; exit 1; }

OUT="$ROOT/dist"
BUILD="$OUT/build-$FLAVOR"
rm -rf "$BUILD"
mkdir -p "$BUILD"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none"

# The sketch profile pins the platform and libraries and produces the merged
# image itself. It needs the package index reachable (or cached, as on any
# machine that has built with it once). Where it is not — the development
# container's egress policy blocks it — fall back to the same FQBN against a
# globally installed core, with sketch.yaml set aside so it cannot override -b.
built=0
if [ -f "$SKETCH/sketch.yaml" ]; then
  if arduino-cli compile --clean --profile esp32s3-ospi \
       --build-property "compiler.cpp.extra_flags=$FLAGS" \
       --output-dir "$BUILD" "$SKETCH" 2>&1 | grep -E "Sketch uses|error:|Error"; then
    [ -f "$BUILD/ats-mini.ino.bin" ] && built=1
  fi
fi
if [ "$built" = 0 ]; then
  restore() { [ -f "$SKETCH/sketch.yaml.off" ] && mv "$SKETCH/sketch.yaml.off" "$SKETCH/sketch.yaml"; }
  trap restore EXIT
  [ -f "$SKETCH/sketch.yaml" ] && mv "$SKETCH/sketch.yaml" "$SKETCH/sketch.yaml.off"
  arduino-cli compile --clean --fqbn "$FQBN" --libraries "$ROOT/lib" \
    --build-property "compiler.cpp.extra_flags=$FLAGS" \
    --output-dir "$BUILD" "$SKETCH" 2>&1 | grep -E "Sketch uses|error:|Error" || true
  restore
  trap - EXIT
fi
[ -f "$BUILD/ats-mini.ino.bin" ] || { echo "release: build failed" >&2; exit 1; }

MERGED="$OUT/airtime-$VER-$FLAVOR.bin"
APP="$OUT/airtime-$VER-$FLAVOR-app.bin"
cp "$BUILD/ats-mini.ino.bin" "$APP"

if [ -f "$BUILD/ats-mini.ino.merged.bin" ]; then
  cp "$BUILD/ats-mini.ino.merged.bin" "$MERGED"
else
  # The ESP32 core ships its own esptool; prefer that over whatever is on PATH,
  # because it is the one that matches the core the image was built with.
  ESPTOOL=""
  for c in "$(ls -d "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1)" \
           "$(command -v esptool 2>/dev/null)" "$(command -v esptool.py 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && { ESPTOOL="$c"; break; }
  done
  [ -n "$ESPTOOL" ] || { echo "release: no esptool to merge with; app image is $APP" >&2; exit 1; }
  "$ESPTOOL" --chip esp32s3 merge-bin -o "$MERGED" \
    --flash-mode dio --flash-freq 80m --flash-size 8MB \
    0x0    "$BUILD/ats-mini.ino.bootloader.bin" \
    0x8000 "$BUILD/ats-mini.ino.partitions.bin" \
    0x10000 "$BUILD/ats-mini.ino.bin"
fi

echo "release: $MERGED  (flash at 0x0, first install)"
echo "release: $APP  (flash at 0x10000, update)"
