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
  if grep -q " $REG " /proc/mounts 2>/dev/null; then
    log "registry already overridden"
  else
    log "refreshing GStreamer registry"
    export GST_REGISTRY_1_0=/tmp/gst_1_0_registry.arm.bin
    /usr/bin/gst-inspect-1.0 > /var/tmp/gst-inspect.log 2>&1
    chmod 644 "$GST_REGISTRY_1_0" 2>/dev/null
    chown :compositor "$GST_REGISTRY_1_0" 2>/dev/null
    mount -n --bind "$GST_REGISTRY_1_0" "$REG" || log "WARN: registry bind failed"
  fi
fi

# 3. Raise avdec_dca priority and apply the stereo downmix coefficients.
if [ ! -f /tmp/gstcool.conf ] && [ -f /etc/gst/gstcool.conf ]; then
  log "overriding /etc/gst/gstcool.conf"
  front=1.25; center=0.75; lfe=0.75; rear=0.75; rear2=0.70
  [ -f "$PAYLOAD_DIR/downmix.conf" ] && . "$PAYLOAD_DIR/downmix.conf"
  sed "s/avdec_dca=0/avdec_dca=290/" /etc/gst/gstcool.conf > /tmp/gstcool.conf
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

# Always succeed: a non-zero webosbrew init script trips the failsafe that
# disables ALL root customisations on the next boot.
exit 0
