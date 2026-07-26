#!/bin/bash
# Compile the AirTime firmware: vendored ats-mini + the AirTime integration.
#
# Usage:
#   tools/build_fw.sh            # AirTime build (-DAIRTIME)
#   tools/build_fw.sh stock      # pristine upstream build, no AirTime code
#   tools/build_fw.sh probe      # Milestone 0 IO11 probe build
#
# Why not `arduino-cli compile --profile esp32s3-ospi` like the Mac runbook?
# sketch.yaml's build profiles resolve their pinned platform+libraries from
# index URLs at build time. In the development container those hosts
# (downloads.arduino.cc, espressif.github.io) are blocked by egress policy, so
# the same platform 3.3.11 and the same library versions are pre-installed
# globally from their github.com homes instead (see docs/STATUS.md). A present
# sketch.yaml with default_profile overrides -b and drags the blocked URLs
# back in — so it is set aside for the duration of the build. On a normal
# network (the owner's Mac), keep using the profile per docs/MILESTONE0.md.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="$ROOT/firmware/ats-mini/ats-mini"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none"

case "${1:-airtime}" in
  airtime) FLAGS="-DAIRTIME" ;;
  stock)   FLAGS="" ;;
  probe)   FLAGS="-DAIRTIME_IO11_PROBE" ;;
  survey)  FLAGS="-DAIRTIME_RDS_SURVEY" ;;
  *) echo "usage: $0 [airtime|stock|probe|survey]" >&2; exit 2 ;;
esac

restore() { [ -f "$SKETCH/sketch.yaml.off" ] && mv "$SKETCH/sketch.yaml.off" "$SKETCH/sketch.yaml"; }
trap restore EXIT
[ -f "$SKETCH/sketch.yaml" ] && mv "$SKETCH/sketch.yaml" "$SKETCH/sketch.yaml.off"

arduino-cli compile \
  --fqbn "$FQBN" \
  --libraries "$ROOT/lib" \
  ${FLAGS:+--build-property "compiler.cpp.extra_flags=$FLAGS"} \
  --warnings default \
  "$SKETCH" 2>&1 | grep -vE "^Error initializing instance"
