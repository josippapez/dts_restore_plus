#!/bin/sh
# hdroute.sh - route dtsdec's DTS-HD bitstream to eARC on webOS 25, from userspace.
#
# LG never connects a software decoder's ES stream to eARC. While dtsdec has an
# HD stream armed (its pid in $ARMED), this connects the stock DSP modules the
# way LG's own bypass would:
#   ES lipsync -> delay -> gain -> bypass (dsp2) -> eARC queue (dsp1)
# replacing the mixer feed of the eARC queue, and puts everything back when the
# stream ends, dtsdec dies, or this service is stopped.
#
# Connections go through /dev/lg/audio_uimc (hdroute-uimc); modules are found in
# the live graph, never by fixed id. Started and stopped by init_dts25.sh
# (W25_HD=start|stop) while the passthrough-hd marker exists. Usage:
#   sh hdroute.sh            run (foreground)
#   sh hdroute.sh --restore  undo a route left by a killed instance, then exit
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
HELPER=$DIR/hdroute-uimc
RUN=/run/dts25
ARMED=$RUN/hd-armed
PIDFILE=$RUN/hdroute.pid
STATE=$RUN/hdroute.state
WORK=/tmp/dts25-hdroute
LOG=/var/lib/webosbrew/dtsenabler/hdroute.log
ACTIVE=0
SEQ=0

mkdir -p "$RUN" "$WORK" "$(dirname "$LOG")"
log() {
  echo "$(date '+%F %T') $*" >> "$LOG"
  # keep the persistent log small
  [ "$(wc -c < "$LOG")" -gt 65536 ] && tail -c 32768 "$LOG" > "$LOG.tmp" && mv -f "$LOG.tmp" "$LOG"
}
# dot_registry of one DSP into $WORK/$2.{graph,edges}; edges are "out port in port"
dump() {
  SEQ=$((SEQ + 1))
  tag=HDROUTE_$$_${SEQ}
  printf '%s debug_aimc2 %s fcall dot_registry\n' "$1" "$tag" > /proc/aaudio/callv
  sleep 1
  dmesg | awk -v t="$tag" 'index($0, t ": start"){a=1} a{print} index($0, t ": end"){a=0; d=1} END{exit !d}' \
    > "$WORK/$2.graph" || return 1
  sed -n 's/.*]  *\([a-z0-9_]*\)_[^_ ]*_out\([0-9a-f]*\) -> \([a-z0-9_]*\)_[^_ ]*_in\([0-9a-f]*\) .*/\1 \2 \3 \4/p' \
    "$WORK/$2.graph" > "$WORK/$2.edges"
}
prop() { echo "$2 $3" > "/proc/aaudio/instances/$1/prop"; }
# gain's mute-<in|out>put from the dumped graph, as 1/0
mute() {
  awk -v g="gain_${GAIN}_props" -v k="mute-$1put=(bool)" 'index($0, g) {
    s = substr($0, index($0, k) + length(k)); print (substr(s, 1, 4) == "true") ? 1 : 0; exit }' "$WORK/d2.graph"
}
one() { [ -n "$1" ] && [ "$(printf '%s\n' "$1" | wc -l)" -eq 1 ]; }
armed() {
  p=$(cat "$ARMED" 2>/dev/null) || return 1
  [ -n "$p" ] && [ "$p" -gt 0 ] 2>/dev/null && [ -d "/proc/$p" ]
}
last_arc() { dmesg | grep '_cb_output_status' | grep ' ARC ' | tail -1 | sed 's/.*ARC \([a-z]*\) .*/\1/'; }

restore() {
  [ -f "$STATE" ] || return 0
  . "$STATE"
  "$HELPER" disconnect-input "0x$QUEUE" 0 >/dev/null 2>&1
  "$HELPER" connect "0x$MIXOUT" 0 "0x$QUEUE" 0 >/dev/null || log "WARN could not reconnect mixerbin_$MIXOUT -> queue_$QUEUE"
  # dtsdec's teardown usually removes the ES input first; then this has nothing to do
  "$HELPER" disconnect-input "0x$DELAY" 0 >/dev/null 2>&1
  prop "$GAIN" mute-input "$GAIN_MUTE_IN"
  prop "$GAIN" mute-output "$GAIN_MUTE_OUT"
  rm -f "$STATE"
  ACTIVE=0
  log "restored mixerbin_$MIXOUT -> queue_$QUEUE"
}

connect() {
  dump 2 d2 && dump 1 d1 || { log 'discovery failed'; return 1; }
  ES=$(awk '$1 ~ /^queue_0418/ && $3 ~ /^lipsync_/ {sub(/^lipsync_/,"",$3); print $3}' "$WORK/d2.edges")
  BYPASS=$(awk '$3 ~ /^bypass_/ {sub(/^bypass_/,"",$3); print $3}' "$WORK/d2.edges")
  GAIN=$(awk -v b="bypass_$BYPASS" '$3==b {sub(/^gain_/,"",$1); print $1}' "$WORK/d2.edges")
  DELAY=$(awk -v g="gain_$GAIN" '$3==g && $1 ~ /^delay_/ {sub(/^delay_/,"",$1); print $1}' "$WORK/d2.edges")
  OUTCTRL=$(awk '$3 ~ /^output_arc_/ {sub(/^outctrlbin_/,"",$1); print $1}' "$WORK/d1.edges")
  QUEUE=$(awk -v o="outctrlbin_$OUTCTRL" '$3==o && $1 ~ /^queue_/ {sub(/^queue_/,"",$1); print $1}' "$WORK/d1.edges")
  MIXOUT=$(awk '$1 ~ /^mixerbin_/ && $2=="0" && $3 ~ /^gluepw_/ {sub(/^mixerbin_/,"",$1); print $1}' "$WORK/d2.edges" | sort -u)
  for v in ES BYPASS GAIN DELAY OUTCTRL QUEUE MIXOUT; do
    eval val=\$$v
    one "$val" || { log "unexpected graph: $v='$val'"; return 1; }
  done
  if grep -q "^lipsync_$ES " "$WORK/d2.edges" || grep -q " 0 delay_$DELAY 0$" "$WORK/d2.edges" ||
      grep -q "^bypass_$BYPASS " "$WORK/d2.edges"; then
    log 'bypass path already in use; not touching it'; return 1
  fi
  GAIN_MUTE_IN=$(mute in)
  GAIN_MUTE_OUT=$(mute out)
  [ -n "$GAIN_MUTE_IN" ] && [ -n "$GAIN_MUTE_OUT" ] || { log 'gain mute unreadable'; return 1; }
  # written before the first change, so a crash mid-connect can still be undone
  printf 'MIXOUT=%s\nQUEUE=%s\nDELAY=%s\nGAIN=%s\nGAIN_MUTE_IN=%s\nGAIN_MUTE_OUT=%s\n' \
    "$MIXOUT" "$QUEUE" "$DELAY" "$GAIN" "$GAIN_MUTE_IN" "$GAIN_MUTE_OUT" > "$STATE.tmp" && mv -f "$STATE.tmp" "$STATE"
  ACTIVE=1
  if ! "$HELPER" connect "0x$ES" 0 "0x$DELAY" 0 >/dev/null ||
      ! "$HELPER" disconnect-input "0x$QUEUE" 0 >/dev/null ||
      ! "$HELPER" connect "0x$BYPASS" 0 "0x$QUEUE" 0 >/dev/null; then
    log 'connect failed; restoring'
    restore
    return 1
  fi
  prop "$GAIN" mute-input 0
  prop "$GAIN" mute-output 0
  log "routed lipsync_$ES -> delay_$DELAY -> gain_$GAIN -> bypass_$BYPASS -> queue_$QUEUE"
}

if [ "${1:-}" = --restore ]; then
  restore
  exit 0
fi
[ -x "$HELPER" ] || { log "missing $HELPER"; exit 1; }
p=$(cat "$PIDFILE" 2>/dev/null)
if [ -n "$p" ] && [ "$p" != $$ ] && [ -d "/proc/$p" ]; then
  exit 0
fi
echo $$ > "$PIDFILE"
trap 'restore; rm -f "$PIDFILE" "$ARMED"; log stopped; exit 0' INT TERM HUP
restore
log "started (pid $$)"
FAILS=0
while :; do
  if armed; then
    if [ "$ACTIVE" = 0 ]; then
      if connect; then FAILS=0; else FAILS=$((FAILS + 1)); fi
      # one stream that cannot be routed is retried a few times, then left alone
      while [ "$FAILS" -ge 5 ] && armed; do sleep 1; done
      armed || FAILS=0
    elif [ "$(last_arc)" = pcm ]; then
      log 'eARC was reset to PCM while armed; rebuilding'
      restore
      connect || :
    fi
  elif [ "$ACTIVE" = 1 ]; then
    restore
  fi
  sleep 1
done
