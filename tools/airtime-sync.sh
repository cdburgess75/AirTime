#!/bin/bash
# Take the computer's clock from the AirTime radio. No arguments needed.
#
#   ./airtime-sync.sh              sync once, now
#   ./airtime-sync.sh --watch      sync now, then every 10 minutes until Ctrl-C
#   sudo ./airtime-sync.sh --install    install a background agent (macOS)
#   sudo ./airtime-sync.sh --uninstall  remove it
#
# WSJT-X and JS8Call have no NTP client of their own — they read the operating
# system's clock (PLAN.md §4). So this is the whole client side of AirTime.
#
# ── Two rules this script exists to enforce ─────────────────────────────────
#
# 1. NEVER sync from a radio that says it is unsynchronised. AirTime reports
#    its own uncertainty honestly; a warm-booted radio that has not heard a
#    source yet will answer with a last-known time and a ± measured in hours.
#    Taking that would be worse than leaving the laptop alone. We read the ±
#    and refuse above --max-unc (default 1 s, which is also FT8's own limit).
#
# 2. DO NOTHING when the radio is not there. That is what makes the background
#    agent safe to install once and forget: at home on the internet it exits
#    silently every ten minutes and your normal time sync is untouched; in the
#    field with AirTime powered up, your laptop simply tracks the radio.
#
# Deliberately does NOT disable the OS's own time sync. With no internet — the
# case AirTime is built for — the OS daemon cannot reach anything and will not
# fight us. With internet, its source is at least as good as ours and it should
# win. Pass --exclusive if you want it switched off anyway (macOS only).

set -u

RADIO="${AIRTIME_RADIO:-192.168.4.1}"
MAX_UNC="1.0"       # seconds; refuse to sync from a radio less certain than this
INTERVAL=600        # --watch / agent period, seconds
FORCE=0
EXCLUSIVE=0
PLIST=/Library/LaunchDaemons/com.airtime.timesync.plist

die() { echo "airtime-sync: $*" >&2; exit 1; }

usage() {
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
}

# Ask the radio for the time. Echoes "offset uncertainty" or nothing.
query() {
  local out
  out=$(sntp "$RADIO" 2>/dev/null) || return 1
  # sntp prints e.g. "-0.005106 +/- 0.070630 192.168.4.1 192.168.4.1", possibly
  # after other lines. Take the last line that carries the +/- field.
  echo "$out" | awk '/\+\/-/ { off=$1; unc=$3 } END { if (unc != "") print off, unc }'
}

# Is a > b?  (floats, no bc dependency)
gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 > b+0) }'; }
abs() { awk -v a="$1" 'BEGIN { printf "%.6f", (a<0 ? -a : a) }'; }

sync_once() {
  local quiet="${1:-0}" reading off unc
  reading=$(query) || reading=""
  if [ -z "$reading" ]; then
    [ "$quiet" = 1 ] || echo "no radio at $RADIO (is the AirTime WiFi joined?)"
    return 2
  fi

  off=$(echo "$reading" | cut -d' ' -f1)
  unc=$(echo "$reading" | cut -d' ' -f2)

  if [ "$FORCE" != 1 ] && gt "$unc" "$MAX_UNC"; then
    echo "radio reports itself uncertain to +/- ${unc}s — refusing to sync."
    echo "it is still acquiring; try again in a few minutes."
    return 3
  fi

  echo "radio is $(abs "$off")s from this computer (radio's own accuracy: +/- ${unc}s)"

  if [ "$(id -u)" != 0 ]; then
    echo "re-running the clock set with sudo..."
    exec sudo -p "password for %u (to set the system clock): " \
      "$0" ${FORCE:+--force} --max-unc "$MAX_UNC" ${quiet:+}
  fi

  case "$(uname -s)" in
    Darwin)
      [ "$EXCLUSIVE" = 1 ] && systemsetup -setusingnetworktime off >/dev/null 2>&1
      sntp -sS "$RADIO" >/dev/null 2>&1 || die "sntp could not set the clock"
      ;;
    Linux)
      if command -v chronyd >/dev/null 2>&1; then
        chronyd -q "server $RADIO iburst" >/dev/null 2>&1 || die "chronyd could not set the clock"
      else
        sntp -sS "$RADIO" >/dev/null 2>&1 || die "sntp could not set the clock"
      fi
      ;;
    *) die "unsupported OS: $(uname -s) — see docs/CLIENT_SETUP.md" ;;
  esac

  reading=$(query) || reading=""
  [ -n "$reading" ] && echo "synced. now within $(abs "$(echo "$reading" | cut -d' ' -f1)")s of the radio."
  return 0
}

install_agent() {
  [ "$(uname -s)" = Darwin ] || die "--install is macOS only; on Linux add '$RADIO iburst prefer' to chrony.conf"
  [ "$(id -u)" = 0 ] || die "--install needs sudo"
  local target=/usr/local/bin/airtime-sync
  mkdir -p /usr/local/bin
  cp "$0" "$target" && chmod 755 "$target" || die "could not install to $target"
  cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.airtime.timesync</string>
  <key>ProgramArguments</key>
  <array><string>$target</string><string>--quiet</string></array>
  <key>StartInterval</key><integer>$INTERVAL</integer>
  <key>RunAtLoad</key><true/>
</dict>
</plist>
PLIST_EOF
  chmod 644 "$PLIST"
  launchctl bootout system "$PLIST" >/dev/null 2>&1
  launchctl bootstrap system "$PLIST" || die "launchctl refused to load $PLIST"
  echo "installed. this machine now tracks the radio whenever AirTime is on the"
  echo "air and reachable, and does nothing at all when it is not."
  echo "remove with: sudo $target --uninstall"
}

uninstall_agent() {
  [ "$(id -u)" = 0 ] || die "--uninstall needs sudo"
  launchctl bootout system "$PLIST" >/dev/null 2>&1
  rm -f "$PLIST" /usr/local/bin/airtime-sync
  echo "removed."
}

MODE=once
QUIET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --watch)     MODE=watch ;;
    --install)   MODE=install ;;
    --uninstall) MODE=uninstall ;;
    --quiet)     QUIET=1 ;;
    --force)     FORCE=1 ;;
    --exclusive) EXCLUSIVE=1 ;;
    --max-unc)   shift; MAX_UNC="${1:-1.0}" ;;
    --interval)  shift; INTERVAL="${1:-600}" ;;
    --radio)     shift; RADIO="${1:-$RADIO}" ;;
    -h|--help)   usage ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

command -v sntp >/dev/null 2>&1 || die "sntp not found — see docs/CLIENT_SETUP.md"

case "$MODE" in
  once)      sync_once "$QUIET" ;;
  watch)     while :; do sync_once "$QUIET"; sleep "$INTERVAL"; done ;;
  install)   install_agent ;;
  uninstall) uninstall_agent ;;
esac
