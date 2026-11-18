#!/bin/sh
# init_dts_c1.sh — boot hook for profile webos6-o20n-gst116-spdif
# (LG OLED C1/G1, webOS 6.x, GStreamer 1.16, 32-bit soft-float userspace).
#
# Verified on an OLED65C17LB (03.53.45) with an optical receiver on 2026-11-17:
# LG Media Player and Moonfin/Emby direct play reach the receiver as DTS.
# Built and qemu-tested on GStreamer 1.16.2. See README.md for the test matrix.
#
# What it does, and why it is reversible:
#   1. Gate: exact platform (GStreamer 1.16.x, ld-linux.so.3, soft-float, W21O
#      OTA id) and no other DTS tool active (CX/C2/webOS 25 hooks or binds).
#   2. Generates a NEW plugin registry in /run (tmpfs) that is LG's stock
#      plugin set plus our dtsdec, and proves it: every element of LG's live
#      registry must still be in it, and dtsdec must load.
#   3. Bind-mounts that registry over LG's media registry. LG's file is never
#      written: `umount` (or a reboot without this hook) restores it exactly.
#   4. If passthrough is opted in, starts the sound-output watcher that tells
#      dtsdec whether an optical/ARC receiver is the live output.
# Nothing under /etc or /usr is changed; no LG service is restarted (each new
# playback starts a fresh media pipeline that reads the registry).
#
# Modes: (none) apply   | stop: remove binds + watcher | status: print state
# Always exits 0 (Homebrew Channel boot-hook discipline).

set -u
STATE=/var/lib/webosbrew/dtsc1
RUN=/run/dtsc1
PLUGDIR=$STATE/plugin
MYSO=$PLUGDIR/libgstdtsdec.so
PT_FLAG=$STATE/passthrough
ENVCONF=$STATE/env.conf
LOG=$STATE/dtsc1.log
NEWREG=$RUN/registry.bin
OUTF=$RUN/sound-output
WPID=$RUN/watcher.pid
OTHER_HOOKS="restore_dts restore_dts25 restore_dts_c2"
INSPECT=/usr/bin/gst-inspect-1.0

mkdir -p "$RUN" 2>/dev/null
log() { echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; }
toast() {
  luna-send -n 1 -a com.webos.service.secondscreen.gateway \
    luna://com.webos.notification/createToast "{\"message\":\"$1\"}" >/dev/null 2>&1 || true
}
# Rotate the log at ~64 KB.
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 65536 ] && mv -f "$LOG" "$LOG.1" 2>/dev/null

REG=
[ -f "$ENVCONF" ] && . "$ENVCONF"     # REG=<LG media registry path>, baked by install

# /proc/mounts lists the resolved path; on a C1 the registry path given by
# the environment is reached through a symlink, so check both spellings.
bound() {
  grep -q " $1 " /proc/mounts 2>/dev/null && return 0
  r=$(readlink -f "$1" 2>/dev/null)
  [ -n "$r" ] && [ "$r" != "$1" ] && grep -q " $r " /proc/mounts 2>/dev/null
}

# Old busybox (webOS 6, seen on a C1) only takes `timeout -t SECS CMD`;
# GNU coreutils and newer busybox take `timeout SECS CMD`.
if timeout -t 1 true >/dev/null 2>&1; then
  tmo() { s=$1; shift; timeout -t "$s" "$@"; }
else
  tmo() { timeout "$@"; }
fi

watcher_stop() {
  if [ -f "$WPID" ]; then
    W=$(cat "$WPID")
    # the watcher, then its luna-send/reader children
    kill "$W" 2>/dev/null
    pkill -P "$W" 2>/dev/null
  fi
  rm -f "$WPID" "$OUTF"
}

do_stop() {
  watcher_stop
  i=0
  while [ -n "$REG" ] && bound "$REG" && [ $i -lt 5 ]; do
    umount -n "$REG" 2>>"$LOG" || umount -n "$(readlink -f "$REG")" 2>>"$LOG" || break
    log "registry bind removed"
    i=$((i + 1))
  done
  [ -n "$REG" ] && ! bound "$REG" && log "LG's registry is live again"
  rm -f "$NEWREG"
}

feature_list() {   # $1 = registry file; prints "plugin:element" lines
  tmo 30 env GST_REGISTRY_1_0="$1" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no \
    "$INSPECT" 2>/dev/null |
    sed -n 's/^\([^ :][^:]*\): *\([^ :][^:]*\):.*/\1:\2/p' | sort -u
}

gate() {
  case "$($INSPECT --version 2>/dev/null | sed -n 's/^GStreamer \([0-9]*\.[0-9]*\).*/\1/p')" in
    1.16) ;; *) echo "GStreamer is not 1.16"; return 1 ;;
  esac
  [ -e /lib/ld-linux.so.3 ] || { echo "no /lib/ld-linux.so.3 (not soft-float armel)"; return 1; }
  case "$(nyx-cmd DeviceInfo query hardware_id 2>/dev/null | head -n1)" in
    HE_DTV_W21O_*) ;; *) echo "not an OLED C1/G1 (W21O) board"; return 1 ;;
  esac
  for h in $OTHER_HOOKS; do
    [ -e "/var/lib/webosbrew/init.d/$h" ] && { echo "another DTS tool is installed ($h)"; return 1; }
  done
  for f in libgstlibav.so libgstmatroska.so libgstisomp4.so libgstmpegtsdemux.so; do
    bound "/usr/lib/gstreamer-1.0/$f" && { echo "another tool overrides $f"; return 1; }
  done
  [ -f "$MYSO" ] || { echo "payload missing ($MYSO)"; return 1; }
  [ -n "$REG" ] && [ -f "$REG" ] || { echo "LG media registry unknown or missing (REG=$REG)"; return 1; }
  return 0
}

watcher_start() {
  [ -f "$PT_FLAG" ] || return 0
  if [ -f "$WPID" ] && kill -0 "$(cat "$WPID")" 2>/dev/null; then return 0; fi
  # The watcher writes "<pid> <soundOutput> <soundOutputDigital>" on every
  # settings change; dtsdec sends the bitstream only while that pid is alive
  # and the output is external_optical/external_arc with passThrough.
  (
    exec 2>/dev/null
    read -r MY _ < /proc/self/stat
    echo "$MY" > "$WPID"
    trap 'rm -f "$OUTF" "$WPID"' EXIT
    trap 'exit 0' INT TERM HUP
    while [ -f "$PT_FLAG" ]; do
      out= digital=
      luna-send -i luna://com.webos.settingsservice/getSystemSettings \
        '{"category":"sound","keys":["soundOutput","soundOutputDigital"],"subscribe":true}' \
        </dev/null | while read -r line; do
          v=$(echo "$line" | sed -n 's/.*"soundOutput" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && out=$v
          v=$(echo "$line" | sed -n 's/.*"soundOutputDigital" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && digital=$v
          [ -n "$out" ] && [ -n "$digital" ] || continue
          echo "$MY $out $digital" > "$OUTF.tmp" && mv -f "$OUTF.tmp" "$OUTF"
        done
      rm -f "$OUTF"        # subscription ended: decode until it is back
      sleep 5
    done
  ) </dev/null >/dev/null 2>&1 &
  log "sound-output watcher started"
}

do_apply() {
  if ! why=$(gate); then
    log "REFUSED: $why"
    do_stop
    echo "REFUSED=$why"
    return 0
  fi

  if bound "$REG"; then
    log "registry already bound; only (re)starting the watcher"
    watcher_start
    echo "APPLIED=already"
    return 0
  fi

  # Plugin directories LG's live registry uses, so the new one is a superset.
  DIRS=$(tmo 60 env GST_REGISTRY_1_0="$REG" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no \
    "$INSPECT" -a 2>/dev/null | sed -n 's/^ *Filename *\(\/.*\)\/[^/]*\.so$/\1/p' | sort -u | tr '\n' ':')
  [ -n "$DIRS" ] || DIRS=/usr/lib/gstreamer-1.0:
  rm -f "$NEWREG"
  if ! tmo 90 env GST_REGISTRY_1_0="$NEWREG" GST_PLUGIN_PATH_1_0="$DIRS$PLUGDIR" \
      GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes "$INSPECT" >/dev/null 2>>"$LOG" \
      || [ ! -s "$NEWREG" ]; then
    log "REFUSED: registry scan failed"; rm -f "$NEWREG"; echo "REFUSED=registry scan failed"; return 0
  fi

  # Proof 1: dtsdec is in the new registry and loads.
  if ! tmo 30 env GST_REGISTRY_1_0="$NEWREG" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no \
      "$INSPECT" dtsdec >/dev/null 2>>"$LOG"; then
    log "REFUSED: dtsdec does not load"; rm -f "$NEWREG"; echo "REFUSED=dtsdec does not load"; return 0
  fi
  # Proof 2: nothing of LG's went missing.
  feature_list "$REG" > "$RUN/stock.list"
  feature_list "$NEWREG" > "$RUN/new.list"
  missing=$(grep -vxF -f "$RUN/new.list" "$RUN/stock.list" | tr '\n' ' ')
  if [ ! -s "$RUN/stock.list" ] || [ -n "$missing" ]; then
    log "REFUSED: new registry lacks LG elements: ${missing:-stock list empty}"
    rm -f "$NEWREG"; echo "REFUSED=registry would lose elements"; return 0
  fi

  chmod 644 "$NEWREG"
  chown :compositor "$NEWREG" 2>/dev/null
  # Read-only: a media process that rescans must not write a registry
  # without dtsdec back over ours (GStreamer's rename onto a bind fails).
  if mount -n --bind "$NEWREG" "$REG" 2>>"$LOG" \
      && { mount -n -o remount,ro,bind "$REG" 2>>"$LOG" || log "WARN: registry bind stays read-write"; }; then
    log "APPLIED: registry with dtsdec bound over $REG ($(wc -l < "$RUN/new.list") elements)"
    watcher_start
    echo "APPLIED=ok"
  else
    log "REFUSED: registry bind failed"; rm -f "$NEWREG"; echo "REFUSED=bind failed"
  fi
}

do_status() {
  echo "profile=webos6-o20n-gst116-spdif"
  echo "registry=$REG bound=$(bound "${REG:-/nonexistent}" && echo yes || echo no)"
  echo "passthrough_optin=$([ -f "$PT_FLAG" ] && echo on || echo off)"
  echo "watcher=$(cat "$OUTF" 2>/dev/null || echo none)"
  echo "last_decision=$(cat "$RUN/last-decision" 2>/dev/null || echo none)"
  echo "plugin_sha256=$(sha256sum "$MYSO" 2>/dev/null | cut -d' ' -f1)"
  tail -n 5 "$LOG" 2>/dev/null
}

case "${1:-}" in
  stop) do_stop ;;
  status) do_status ;;
  *) do_apply ;;
esac
exit 0
