#!/bin/bash
# Advance AIRTIME_VERSION under the vYY.MM.DD.NNN convention: today's date,
# then NNN counting the day's builds from 001.
#
#   tools/bump_version.sh          # rewrites Common.h, prints the new version
#
# Same day  -> the build number increments (v26.07.27.001 -> v26.07.27.002).
# New day   -> the number resets            (v26.07.27.003 -> v26.07.28.001).
#
# Run it before cutting anything an operator will see; the About screen and
# the serial banner both carry this string, and "which firmware is this radio
# actually running" has already cost one false alarm when the only way to tell
# two builds apart was comparing binary sizes.
set -e

COMMON="$(cd "$(dirname "$0")/.." && pwd)/firmware/ats-mini/ats-mini/Common.h"

CUR=$(sed -n 's/.*AIRTIME_VERSION "\(v[^"]*\)".*/\1/p' "$COMMON")
[ -n "$CUR" ] || { echo "bump_version: no AIRTIME_VERSION in Common.h" >&2; exit 1; }

TODAY=$(date +%y.%m.%d)
if [ "${CUR:1:8}" = "$TODAY" ]; then
  # 10# forces base ten: "008" and "009" are octal to the shell otherwise,
  # and 009 is not even valid octal — the ninth build of a day would fail.
  N=$((10#${CUR:10} + 1))
else
  N=1
fi
NEW=$(printf 'v%s.%03d' "$TODAY" "$N")

# -i.bak + rm rather than plain -i: this runs on the owner's Mac as well as in
# the dev container, and BSD sed's -i demands the suffix argument.
sed -i.bak "s/AIRTIME_VERSION \"$CUR\"/AIRTIME_VERSION \"$NEW\"/" "$COMMON"
rm -f "$COMMON.bak"
echo "$NEW"
