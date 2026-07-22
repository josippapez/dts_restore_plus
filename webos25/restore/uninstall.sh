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
