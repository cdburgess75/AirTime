#!/bin/bash
# Build a single flashable image, so somebody can try AirTime without
# installing arduino-cli, an ESP32 core and four libraries.
#
#   tools/release.sh [airtime|fast]   ->  dist/airtime-<version>-<flavor>.bin
#
# The output is a MERGED image: bootloader + partition table + application in
# one file at offset 0. That is what a web flasher (esptool-js, ESP Web Tools)
# expects, and it removes the commonest way a first flash goes wrong — three
# files at three offsets, one of them mistyped.
#
# Flashing the result:
#   esptool.py --chip esp32s3 write_flash 0x0 dist/airtime-<version>-<flavor>.bin
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
mkdir -p "$BUILD"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none"

# Same sketch.yaml dance as build_fw.sh: a present profile overrides -b and
# drags in index URLs this environment cannot reach.
restore() { [ -f "$SKETCH/sketch.yaml.off" ] && mv "$SKETCH/sketch.yaml.off" "$SKETCH/sketch.yaml"; }
trap restore EXIT
[ -f "$SKETCH/sketch.yaml" ] && mv "$SKETCH/sketch.yaml" "$SKETCH/sketch.yaml.off"

arduino-cli compile --clean --fqbn "$FQBN" --libraries "$ROOT/lib" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --output-dir "$BUILD" "$SKETCH" 2>&1 | grep -E "Sketch uses|error" || true

MERGED="$OUT/airtime-$VER-$FLAVOR.bin"

# The ESP32 core ships its own esptool; prefer that over whatever is on PATH,
# because it is the one that matches the core the image was built with.
ESPTOOL=""
for c in "$(ls -d "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1)" \
         "$(command -v esptool.py 2>/dev/null)" "$(command -v esptool 2>/dev/null)"; do
  [ -n "$c" ] && [ -x "$c" ] && { ESPTOOL="$c"; break; }
done

if [ -n "$ESPTOOL" ]; then
  "$ESPTOOL" --chip esp32s3 merge-bin -o "$MERGED" \
    --flash-mode dio --flash-freq 80m --flash-size 8MB \
    0x0    "$BUILD/ats-mini.ino.bootloader.bin" \
    0x8000 "$BUILD/ats-mini.ino.partitions.bin" \
    0x10000 "$BUILD/ats-mini.ino.bin"
  echo "release: $MERGED"
else
  # No esptool here? Still useful — hand over the three parts and their offsets
  # rather than pretending the merge happened.
  echo "release: esptool not found; application image is $BUILD/ats-mini.ino.bin"
  echo "  flash as: 0x0 bootloader.bin  0x8000 partitions.bin  0x10000 ats-mini.ino.bin"
fi
