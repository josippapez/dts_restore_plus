#!/bin/bash
# dts_restore boot-time init script.
# Installed to /var/lib/webosbrew/dts_restore/ and run on every boot via
# /var/lib/webosbrew/init.d/restore_dts (webosbrew's run-parts). Re-applies the
# DTS/DV GStreamer overrides. Safe to run repeatedly - every step is idempotent.
# Copyright (c) 2022-2025 Pete Batard <pete@akeo.ie>
# See https://github.com/RootMyTV/RootMyTV.github.io/issues/72
set -u

PAYLOAD_DIR=$(dirname "$(realpath "$0" 2>/dev/null || echo "$0")")
GST_SRC="$PAYLOAD_DIR/gst"
LOG=/tmp/dts_restore.log

log() { echo "[dts_restore] $*" >> "$LOG" 2>/dev/null; }

# GStreamer registry path captured at install time (boot has no GST env of its own).
BAKED_GST_REGISTRY=""
[ -f "$PAYLOAD_DIR/env.conf" ] && . "$PAYLOAD_DIR/env.conf"

# Kill switch: `touch .../disabled` to skip everything without uninstalling.
if [ -f "$PAYLOAD_DIR/disabled" ]; then
  log "disabled flag present - skipping"
  exit 0
fi

# webOS 6 (OLED C1/G1, GStreamer 1.16): install.sh stages a dtsdec there and
# nowhere else. It decodes DTS, or passes it to an optical/ARC receiver when
# passthrough is on. Both paths below are compiled into that dtsdec.
GST116=""
[ -f "$PAYLOAD_DIR/gst116/libgstdtsdec.so" ] && GST116="$PAYLOAD_DIR/gst116"
PT_FLAG=/var/lib/webosbrew/dtsc1/passthrough
PT_RUN=/run/dtsc1

# 1. Override the GStreamer plugins LG nerfed (guarded against double-mounting).
#    libgstmpegtsdemux.so is included for future m2ts/BD DTS support; the loop
#    silently skips it when the file is absent (it is not in every release).
for lib in libgstisomp4.so libgstisomp4_1_8.so libgstmatroska.so libgstlibav.so libgstmpegtsdemux.so; do
  src="$GST_SRC/$lib"
  target="/usr/lib/gstreamer-1.0/$lib"
  [ -f "$src" ] || continue
  if grep -q " $target " /proc/mounts 2>/dev/null; then
    log "$lib already overridden"
  else
    log "overriding $target"
    mount -n --bind -o ro "$src" "$target" || log "WARN: bind mount failed for $lib"
  fi
done

# 2. Refresh the GStreamer registry (regenerated in /tmp, bound over the real path).
REG="${GST_REGISTRY_1_0:-$BAKED_GST_REGISTRY}"
if [ -n "$REG" ] && [ -f "$REG" ]; then
  # A registry bound by an install without dtsdec is rebuilt.
  if grep -q " $REG " /proc/mounts 2>/dev/null && { [ -z "$GST116" ] || grep -q "$GST116/libgstdtsdec.so" "$REG"; }; then
    log "registry already overridden"
  else
    log "refreshing GStreamer registry"
    export GST_REGISTRY_1_0=/tmp/gst_1_0_registry.arm.bin
    if [ -n "$GST116" ]; then
      # GST_PLUGIN_PATH_1_0 replaces GST_PLUGIN_PATH, so keep the one in use.
      P=${GST_PLUGIN_PATH_1_0-${GST_PLUGIN_PATH-}}
      export GST_PLUGIN_PATH_1_0="$GST116${P:+:$P}"
    fi
    GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 > /var/tmp/gst-inspect.log 2>&1
    chmod 644 "$GST_REGISTRY_1_0" 2>/dev/null
    chown :compositor "$GST_REGISTRY_1_0" 2>/dev/null
    mount -n --bind "$GST_REGISTRY_1_0" "$REG" || log "WARN: registry bind failed"
  fi
fi

# 3. Raise avdec_dca priority and apply the stereo downmix coefficients.
#    A rank file written by an install without dtsdec is rebuilt from LG's.
if [ -n "$GST116" ] && [ -f /tmp/gstcool.conf ] && ! grep -q "^dtsdec=" /tmp/gstcool.conf; then
  umount /etc/gst/gstcool.conf 2>/dev/null
  rm -f /tmp/gstcool.conf
fi
if [ ! -f /tmp/gstcool.conf ] && [ -f /etc/gst/gstcool.conf ]; then
  log "overriding /etc/gst/gstcool.conf"
  front=1.25; center=0.75; lfe=0.75; rear=0.75; rear2=0.70
  [ -f "$PAYLOAD_DIR/downmix.conf" ] && . "$PAYLOAD_DIR/downmix.conf"
  if [ -n "$GST116" ]; then
    # dtsdec is compiled at rank 256: put it above avdec_dca, in the same section.
    sed -e "s/avdec_dca=0/avdec_dca=290/" -e '/avdec_dca=/a\
dtsdec=300' /etc/gst/gstcool.conf > /tmp/gstcool.conf
  else
    sed "s/avdec_dca=0/avdec_dca=290/" /etc/gst/gstcool.conf > /tmp/gstcool.conf
  fi
  {
    echo ""
    echo "[downmix]"
    echo "front=$front"
    echo "center=$center"
    echo "lfe=$lfe"
    echo "rear=$rear"
    echo "rear2=$rear2"
  } >> /tmp/gstcool.conf
  mount -n --bind /tmp/gstcool.conf /etc/gst/gstcool.conf || log "WARN: gstcool bind failed"
fi

# 4. Passthrough opt-in (webOS 6 dtsdec only): a watcher writes
#    "<its pid> <soundOutput> <soundOutputDigital>" on every sound settings
#    change. dtsdec passes DTS through only while that pid is alive and the
#    output is an optical/ARC receiver on Pass Through.
#    The C1's kill -0 "" succeeds, so a missing pid is checked on its own.
pid=$(cat "$PT_RUN/watcher.pid" 2>/dev/null)
if [ -n "$GST116" ] && [ -f "$PT_FLAG" ] && { [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; }; then
  mkdir -p "$PT_RUN"
  (
    read -r MY _ < /proc/self/stat
    echo "$MY" > "$PT_RUN/watcher.pid"
    trap 'rm -f "$PT_RUN/sound-output" "$PT_RUN/watcher.pid"' EXIT
    trap 'exit 0' INT TERM HUP
    while [ -f "$PT_FLAG" ]; do
      out= digital=
      luna-send -i luna://com.webos.settingsservice/getSystemSettings \
        '{"category":"sound","keys":["soundOutput","soundOutputDigital"],"subscribe":true}' \
        </dev/null | while read -r line; do
          v=$(echo "$line" | sed -n 's/.*"soundOutput" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && out=$v
          v=$(echo "$line" | sed -n 's/.*"soundOutputDigital" *: *"\([^"]*\)".*/\1/p'); [ -n "$v" ] && digital=$v
          [ -n "$out" ] && [ -n "$digital" ] || continue
          echo "$MY $out $digital" > "$PT_RUN/sound-output.tmp" && mv -f "$PT_RUN/sound-output.tmp" "$PT_RUN/sound-output"
        done
      rm -f "$PT_RUN/sound-output"   # subscription ended: decode until it is back
      sleep 5
    done
  ) </dev/null >/dev/null 2>&1 &
  log "sound-output watcher started"
fi

# Always succeed: a non-zero webosbrew init script trips the failsafe that
# disables ALL root customisations on the next boot.
exit 0
