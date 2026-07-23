#!/bin/sh
#
# uninstall.sh - Remove the unified webOS 25 DTS + TrueHD/MLP restore.
#
# Run as root ON THE TV: sh uninstall.sh
#
# Reverses install.sh:
#   1. Removes the boot hook symlink.
#   2. Unmounts all bind-mounts, restoring LG's originals:
#        - /etc/umediaserver/device_codec_capability_config.json
#        - /etc/gst/gstcool.conf
#        - /usr/lib/gstreamer-1.0/libgstlibav.so
#        - /usr/lib/gstreamer-1.0/libgstisomp4.so       (container DTS)
#        - /usr/lib/gstreamer-1.0/libgstmpegtsdemux.so  (container DTS)
#        - /mnt/flash/data/gst_1_0_registry.arm.bin
#   3. Removes /var/lib/webosbrew/{dts25,truehd,demux25}.
#   4. Restarts the media pipeline.
#
# The bind-mounts are the only thing that alters live behaviour; unmounting
# them fully reverts the TV. A REBOOT is recommended to guarantee a clean
# state (drops any in-memory registry the media process already cached).
#
# Always exits 0.
#
set -u

LOG=/tmp/dts25.log
DTS_DEST=/var/lib/webosbrew/dts25
THD_DEST=/var/lib/webosbrew/truehd
DMX_DEST=/var/lib/webosbrew/demux25
HOOK=/var/lib/webosbrew/init.d/restore_dts25

CFG_LIVE=/etc/umediaserver/device_codec_capability_config.json
GC_LIVE=/etc/gst/gstcool.conf
LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so
DMX_ISO=/usr/lib/gstreamer-1.0/libgstisomp4.so
DMX_TSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so
REG_TARGET=/mnt/flash/data/gst_1_0_registry.arm.bin
REG_TMP=/tmp/gst_dts_reg.bin

log() { echo "[dts25-uninstall $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

log "=== unified DTS+TrueHD uninstall start ==="

# --- 1. Remove boot hook ---------------------------------------------------
if [ -L "$HOOK" ] || [ -e "$HOOK" ]; then
  rm -f "$HOOK" && log "removed boot hook $HOOK"
else
  log "boot hook not present"
fi

# --- 2. Unmount all binds --------------------------------------------------
for T in "$CFG_LIVE" "$GC_LIVE" "$LGLIBAV" "$DMX_ISO" "$DMX_TSD" "$REG_TARGET"; do
  if grep -q " $T " /proc/mounts 2>/dev/null; then
    if umount "$T" 2>>"$LOG"; then
      log "unmounted bind over $T (reverted to LG original)"
    else
      log "WARN could not unmount $T"
    fi
  else
    log "no bind-mount present over $T"
  fi
done
rm -f "$REG_TMP" 2>/dev/null

# --- 2b. Regenerate a clean STOCK GStreamer registry -----------------------
# The registry is written by init_dts25.sh with `cp -f` (a PERSISTENT overwrite),
# NOT a bind-mount -- so the umount above can never revert it. Left alone, the
# stale registry keeps referencing the /var/lib/webosbrew/* plugins removed in
# step 3, which breaks media-pipeline app audio (e.g. Spotify) even after a
# reboot, until a valid registry is regenerated. (Root-caused on a real C5,
# 2026-07-23.) The binds above are already removed, so regenerate from the
# pristine on-disk stock plugins and overwrite the registry.
CLEAN_REG=/tmp/gst_clean_reg.bin
rm -f "$CLEAN_REG" 2>/dev/null
if GST_REGISTRY_1_0="$CLEAN_REG" \
   GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0 \
   GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes \
   timeout 60 /usr/bin/gst-inspect-1.0 >/dev/null 2>>"$LOG"; then
  cp -f "$CLEAN_REG" "$REG_TARGET" 2>>"$LOG" \
    && log "regenerated clean stock registry (reverted cp-based override)" \
    || log "WARN could not write clean registry"
else
  log "WARN clean registry regen failed; leaving existing registry untouched (may still be stale)"
fi
rm -f "$CLEAN_REG" 2>/dev/null

# --- 3. Remove install dirs ------------------------------------------------
for D in "$DTS_DEST" "$THD_DEST" "$DMX_DEST"; do
  if [ -d "$D" ]; then
    rm -rf "$D" && log "removed $D"
  else
    log "$D not present"
  fi
done

# --- 4. Restart media pipeline ---------------------------------------------
if killall starfish-media-pipeline 2>>"$LOG"; then
  log "restarted starfish-media-pipeline"
else
  log "note: starfish-media-pipeline not running"
fi

log "=== unified DTS+TrueHD uninstall done ==="
echo "DTS + TrueHD uninstall complete (reboot recommended). See $LOG for details."
exit 0
