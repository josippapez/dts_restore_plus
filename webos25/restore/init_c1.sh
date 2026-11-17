#!/bin/sh
# init_c1.sh -- webOS 6 (LG C1, kernel 4.4.84-229) DTS-HD passthrough, EXPERIMENTAL.
#
# Runs only while the opt-in marker exists ($C1_DIR/enabled); the app writes it.
#   sh init_c1.sh          boot or apply: bind dtsdec, start the sound-output watcher, load
#                          earc_route-4.4.ko (it stays until reboot: the 4.4 build has no exit)
#   C1_PT=stop sh init_c1.sh   stop the watcher and unbind dtsdec; the module stays loaded
#                          but holds nothing without the lease, so LG's audio is stock
# Always exits 0 at boot: a non-zero webosbrew init script disables root customisations.
set -u
C1_DIR=/var/lib/webosbrew/dts25/c1
RUN=/run/dts25
LOG=/var/lib/webosbrew/dtsenabler/c1.log
MYDTS=$C1_DIR/libgstdtsdec.so
SINK=/usr/lib/gstreamer-1.0/libgstlxaudiosink.so
# The CX profile's registry: init_dts.sh regenerates it and binds it over LG's at boot.
REG_TMP=/tmp/gst_1_0_registry.arm.bin
REG_LIVE=${GST_REGISTRY_1_0:-}
[ -n "$REG_LIVE" ] || { [ -f /var/lib/webosbrew/dtsenabler/cx/env.conf ] && . /var/lib/webosbrew/dtsenabler/cx/env.conf; REG_LIVE=${BAKED_GST_REGISTRY:-}; }

c1_log() { mkdir -p "$(dirname "$LOG")" && echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null; }

# LG's C1 audio sink, 03.53.45 (W21O and W21K): dtsdec arms the HD route only behind it.
c1_sink_verified() {
  case "$1" in
    a3e8dc6ad3ed584c29008ca48579eed94ac7b27284982648f9d65d2a32d26f8d) return 0 ;;
  esac
  return 1
}

# Same file and format as webOS 25 (lgroute-control.c reads it): "<pid> <soundOutput> <digital>".
c1_watch() {
  read -r WPID _ < /proc/self/stat
  echo "$WPID" > "$RUN/watcher.pid"
  trap 'rm -f "$RUN/sound-output" "$RUN/watcher.pid"' EXIT
  trap 'exit 0' INT TERM HUP
  while [ -f "$C1_DIR/enabled" ] && [ ! -e "$RUN/stopping" ]; do
    luna-send -i luna://com.webos.settingsservice/getSystemSettings \
      '{"category":"sound","keys":["soundOutput","soundOutputDigital"],"subscribe":true}' \
      </dev/null 2>/dev/null | {
      out='' digital=''
      while read -r line; do
        [ -f "$C1_DIR/enabled" ] && [ ! -e "$RUN/stopping" ] || break
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

c1_stop() {
  : > "$RUN/stopping" 2>/dev/null
  P=$(cat "$RUN/watcher.pid" 2>/dev/null)
  [ -n "$P" ] && kill "$P" 2>/dev/null
  rm -f "$RUN/sound-output" "$RUN/watcher.pid" "$RUN/sink-ok" /var/lib/webosbrew/dts25/passthrough \
    /var/lib/webosbrew/dts25/passthrough-hd
  # dtsdec stays registered until the next boot but decodes again: its markers are gone.
  c1_log "stopped; dtsdec decodes again, earc_route stays loaded until reboot, idle without the lease"
}

if [ "${C1_PT:-}" = stop ]; then c1_stop; exit 0; fi
[ -f "$C1_DIR/enabled" ] || exit 0
[ "$(uname -r)" = 4.4.84-229.kcl4tv.6 ] || { c1_log "refused: kernel $(uname -r) is not 4.4.84-229.kcl4tv.6"; exit 0; }
[ -f "$MYDTS" ] || { c1_log "refused: $MYDTS missing"; exit 0; }
mkdir -p "$RUN" && rm -f "$RUN/stopping"

# dtsdec only arms the HD route when both markers exist and the sink is the analysed build.
: > /var/lib/webosbrew/dts25/passthrough
: > /var/lib/webosbrew/dts25/passthrough-hd
SHA=$(sha256sum "$SINK" 2>/dev/null)
if c1_sink_verified "${SHA%% *}"; then : > "$RUN/sink-ok"
else c1_log "HD refused: untested LG sink build (${SHA%% *})"; rm -f "$RUN/sink-ok"; fi

# LG ships no dtsdec on the C1. As on webOS 25, the registry is rescanned with our directory
# added and bound over LG's, so the read-only /usr is never written.
if [ -n "$REG_LIVE" ] && [ -f "$REG_LIVE" ]; then
  GST_REGISTRY_1_0=$REG_TMP.c1 GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes \
    GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:$C1_DIR \
    timeout 60 /usr/bin/gst-inspect-1.0 dtsdec >/dev/null 2>>"$LOG"
  if GST_REGISTRY_1_0=$REG_TMP.c1 GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 dtsdec >/dev/null 2>&1; then
    mv -f "$REG_TMP.c1" "$REG_TMP" && chmod 644 "$REG_TMP" && chown :compositor "$REG_TMP" 2>/dev/null
    grep -q " $REG_LIVE " /proc/mounts 2>/dev/null || mount -n --bind "$REG_TMP" "$REG_LIVE"
    c1_log "registered dtsdec in $REG_LIVE"
  else
    rm -f "$REG_TMP.c1"
    c1_log "WARN dtsdec did not register; DTS stays on the CX decoder"
  fi
else
  c1_log "WARN no GStreamer registry path (enable DTS first); DTS stays on the CX decoder"
fi

P=$(cat "$RUN/watcher.pid" 2>/dev/null)
if [ -z "$P" ] || [ ! -d "/proc/$P" ]; then ( c1_watch ) </dev/null >/dev/null 2>&1 & fi

if [ ! -d /sys/module/earc_route ]; then
  OUT=$(sh "$C1_DIR/earc-route-load.sh" --dts-hd 2>&1 </dev/null)
  c1_log "earc_route: $OUT"
fi
exit 0
