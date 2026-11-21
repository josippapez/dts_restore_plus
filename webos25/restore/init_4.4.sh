#!/bin/sh
# init_4.4.sh -- kernel 4.4.84-229 (webOS 6) DTS-HD passthrough, EXPERIMENTAL.
#
# Runs only while the opt-in marker exists ($HD44_DIR/enabled); the app writes it.
#   sh init_4.4.sh          boot or apply: bind dtsdec, start the sound-output watcher, load
#                          earc_route-4.4.ko (it stays until reboot: the 4.4 build has no exit)
#   HD44=stop sh init_4.4.sh   stop the watcher and unbind dtsdec; the module stays loaded
#                          but holds nothing without the lease, so LG's audio is stock
# Always exits 0 at boot: a non-zero webosbrew init script disables root customisations.
set -u
HD44_DIR=/var/lib/webosbrew/dts25/4.4
RUN=/run/dts25
LOG=/var/lib/webosbrew/dtsenabler/hd-4.4.log
MYDTS=$HD44_DIR/libgstdtsdec.so
SINK=/usr/lib/gstreamer-1.0/libgstlxaudiosink.so
SINK_SHA256=a3e8dc6ad3ed584c29008ca48579eed94ac7b27284982648f9d65d2a32d26f8d
# The CX profile's registry: init_dts.sh regenerates it and binds it over LG's at boot.
REG_TMP=/tmp/gst_1_0_registry.arm.bin
REG_LIVE=${GST_REGISTRY_1_0:-}
[ -n "$REG_LIVE" ] || { [ -f /var/lib/webosbrew/dtsenabler/cx/env.conf ] && . /var/lib/webosbrew/dtsenabler/cx/env.conf; REG_LIVE=${BAKED_GST_REGISTRY:-}; }

hd44_log() { mkdir -p "$(dirname "$LOG")" && echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; }

# Same file and format as webOS 25 (lgroute-control.c reads it): "<pid> <soundOutput> <digital>".
hd44_watch() {
  read -r WPID _ < /proc/self/stat
  echo "$WPID" > "$RUN/watcher.pid"
  trap 'rm -f "$RUN/sound-output" "$RUN/watcher.pid"' EXIT
  trap 'exit 0' INT TERM HUP
  while [ -f "$HD44_DIR/enabled" ] && [ ! -e "$RUN/stopping" ]; do
    luna-send -i luna://com.webos.settingsservice/getSystemSettings \
      '{"category":"sound","keys":["soundOutput","soundOutputDigital"],"subscribe":true}' \
      </dev/null 2>/dev/null | {
      out='' digital=''
      while read -r line; do
        [ -f "$HD44_DIR/enabled" ] && [ ! -e "$RUN/stopping" ] || break
        v=$(echo "$line" | sed -n 's/.*"soundOutput" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && out=$v
        v=$(echo "$line" | sed -n 's/.*"soundOutputDigital" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && digital=$v
        [ -n "$out" ] && [ -n "$digital" ] || continue
        echo "$WPID $out $digital" > "$RUN/sound-output.tmp" && mv -f "$RUN/sound-output.tmp" "$RUN/sound-output"
      done
    }
    rm -f "$RUN/sound-output"
    sleep 5
  done
}

hd44_stop() {
  : > "$RUN/stopping" 2>/dev/null
  P=$(cat "$RUN/watcher.pid" 2>/dev/null)
  [ -n "$P" ] && kill "$P" 2>/dev/null
  rm -f "$RUN/sound-output" "$RUN/watcher.pid" "$RUN/sink-ok" /var/lib/webosbrew/dts25/passthrough \
    /var/lib/webosbrew/dts25/passthrough-hd
  [ -f /tmp/gstcool.conf ] && sed '/^dtsdec=/d' /tmp/gstcool.conf > /tmp/gstcool.conf.c1 \
    && cat /tmp/gstcool.conf.c1 > /tmp/gstcool.conf; rm -f /tmp/gstcool.conf.c1
  # dtsdec stays registered until the next boot but decodes again: its markers are gone.
  hd44_log "stopped; dtsdec decodes again, earc_route stays loaded until reboot, idle without the lease"
}

if [ "${HD44:-}" = stop ]; then hd44_stop; exit 0; fi
[ -f "$HD44_DIR/enabled" ] || exit 0
[ "$(uname -r)" = 4.4.84-229.kcl4tv.6 ] || { hd44_log "refused: kernel $(uname -r) is not 4.4.84-229.kcl4tv.6"; exit 0; }
[ -f "$MYDTS" ] || { hd44_log "refused: $MYDTS missing"; exit 0; }
mkdir -p "$RUN" && rm -f "$RUN/stopping"

# dtsdec only arms the HD route when both markers exist and the sink is the analysed build.
: > /var/lib/webosbrew/dts25/passthrough
: > /var/lib/webosbrew/dts25/passthrough-hd
SHA=$(sha256sum "$SINK" 2>/dev/null)
if [ "${SHA%% *}" = "$SINK_SHA256" ]; then : > "$RUN/sink-ok"
else hd44_log "HD refused: untested LG sink build (${SHA%% *})"; rm -f "$RUN/sink-ok"; fi

# LG ships no dtsdec on webOS 6. As on webOS 25, the registry is rescanned with our directory
# added and bound over LG's, so the read-only /usr is never written.
if [ -n "$REG_LIVE" ] && [ -f "$REG_LIVE" ]; then
  GST_REGISTRY_1_0=$REG_TMP.c1 GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes \
    GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:$HD44_DIR \
    timeout 60 /usr/bin/gst-inspect-1.0 dtsdec >/dev/null 2>>"$LOG"
  if GST_REGISTRY_1_0=$REG_TMP.c1 GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 dtsdec >/dev/null 2>&1; then
    mv -f "$REG_TMP.c1" "$REG_TMP" && chmod 644 "$REG_TMP" && chown :compositor "$REG_TMP" 2>/dev/null
    # mv gives $REG_TMP a new inode and an existing bind keeps showing the old one: bind again.
    mount -n --bind "$REG_TMP" "$REG_LIVE"
    hd44_log "registered dtsdec in $REG_LIVE"
  else
    rm -f "$REG_TMP.c1"
    hd44_log "WARN dtsdec did not register; DTS stays on the CX decoder"
  fi
else
  hd44_log "WARN no GStreamer registry path (enable DTS first); DTS stays on the CX decoder"
fi

# LG's libgstcool applies gstcool.conf ranks; the CX hook raised avdec_dca to 290, above
# dtsdec's own 256. The hook binds /tmp/gstcool.conf, so it is rewritten in place (sed -i
# would swap the inode out from under the bind).
GC_TMP=/tmp/gstcool.conf
if [ -f "$GC_TMP" ] && ! grep -q '^dtsdec=' "$GC_TMP"; then
  sed 's/^\[rank\]$/[rank]\ndtsdec=300/' "$GC_TMP" > "$GC_TMP.c1" && cat "$GC_TMP.c1" > "$GC_TMP"
  rm -f "$GC_TMP.c1"
  grep -q '^dtsdec=300$' "$GC_TMP" && hd44_log "dtsdec ranked 300, above avdec_dca" \
    || hd44_log "WARN could not rank dtsdec in $GC_TMP"
fi

P=$(cat "$RUN/watcher.pid" 2>/dev/null)
if [ -z "$P" ] || [ ! -d "/proc/$P" ]; then ( hd44_watch ) </dev/null >/dev/null 2>&1 & fi

if [ ! -d /sys/module/earc_route ]; then
  OUT=$(sh "$HD44_DIR/earc-route-load.sh" --dts-hd 2>&1 </dev/null)
  hd44_log "earc_route: $OUT"
fi
exit 0
