#!/bin/sh
#
# uninstall.sh - Remove the unified webOS 25 DTS + TrueHD/MLP restore.
#
# Run as root ON THE TV: sh uninstall.sh
#
# Reverses install.sh:
#   1. Removes the boot hook symlink.
#   2. Unmounts all bind-mounts (lazy detach as a fallback when a live mapping
#      such as WebAppMgr holds one busy), restoring LG's originals:
#        - /etc/umediaserver/device_codec_capability_config.json
#        - /etc/gst/gstcool.conf
#        - /usr/lib/gstreamer-1.0/libgstlibav.so
#        - /usr/lib/gstreamer-1.0/libgstisomp4.so       (container DTS)
#        - /usr/lib/gstreamer-1.0/libgstmpegtsdemux.so  (container DTS)
#        - /mnt/flash/data/gst_1_0_registry.arm.bin
#   2c. Regenerates a clean STOCK registry -- and this gates step 3.
#   3. Removes dts25/stock.fp and /var/lib/webosbrew/{dts25,truehd,demux25},
#      but ONLY if 2c succeeded. If the registry could not be rebuilt the staged
#      files are kept on purpose (deleting plugins the live registry still names
#      breaks other apps' audio) and the script says so loudly; re-run it.
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
# The pristine stock fingerprints recorded by init_dts25.sh. It lives under
# $DTS_DEST, which step 3 removes wholesale; removing it explicitly keeps the
# intent visible and still holds if that directory removal fails.
STOCK_FP=$DTS_DEST/stock.fp

log() { echo "[dts25-uninstall $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

# Unmount one bind target, falling back to a LAZY detach when it is busy.
# Measured on a real C5: umount of /usr/lib/gstreamer-1.0/libgstlibav.so fails
# with "target is busy" because WebAppMgr (pid 3492) has the .so mapped, while
# `umount -l` succeeds. Without this fallback an uninstall only logged a WARN and
# silently left the libav override applied -- i.e. it was not reversible at all.
# Same semantics as w25_umount() in init_dts25.sh.
w25_umount() {
  grep -q " $1 " /proc/mounts 2>/dev/null || { log "no bind-mount present over $1"; return 0; }
  if umount "$1" 2>>"$LOG"; then log "unmounted bind over $1 (reverted to LG original)"; return 0; fi
  if umount -l "$1" 2>>"$LOG"; then log "lazy-detached busy bind over $1 (a live mapping held it)"; return 0; fi
  log "WARN could not unmount $1, even lazily"
  return 1
}

log "=== unified DTS+TrueHD uninstall start ==="

# --- 1. Remove boot hook ---------------------------------------------------
if [ -L "$HOOK" ] || [ -e "$HOOK" ]; then
  rm -f "$HOOK" && log "removed boot hook $HOOK"
else
  log "boot hook not present"
fi

# --- 2. Unmount all binds --------------------------------------------------
# Rootfs paths only. The same binds propagate into every app jail (27 jail-side
# copies per library on a real C5); those are left alone deliberately, because
# detaching a jail's own view of the library would break that jail.
for T in "$CFG_LIVE" "$GC_LIVE" "$LGLIBAV" "$DMX_ISO" "$DMX_TSD" "$REG_TARGET"; do
  w25_umount "$T"
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
#
# THIS DECIDES WHETHER STEP 3 MAY DELETE ANYTHING. Regenerating LG's registry is
# a cold-cache full plugin scan under `timeout`, so it can genuinely fail. If it
# does and we deleted our plugins anyway, the live registry would name plugins
# that no longer exist -- the exact state that broke other apps' audio, and one
# LG's stack does not recover from on its own. So: regenerate first, delete only
# on success, and otherwise leave a working install in place for the user to
# retry. Same rule as w25_self_heal() in init_dts25.sh.
REG_OK=0
# Refuse to rebuild while any plugin bind of ours is still mounted: the scan would
# read OUR library at the stock path (its RUNPATH resolves), write a registry that
# is syntactically valid and mentions no /var/lib/webosbrew path, and look like
# proof the TV is back on stock -- so step 3 would delete the payload out from
# under a surviving bind. Same precondition as w25_stock_registry() in
# init_dts25.sh.
BINDS_LEFT=0
for T in "$LGLIBAV" "$DMX_ISO" "$DMX_TSD"; do
  grep -q " $T " /proc/mounts 2>/dev/null && BINDS_LEFT=1
done
CLEAN_REG=/tmp/gst_clean_reg.bin
rm -f "$CLEAN_REG" 2>/dev/null
if [ "$BINDS_LEFT" = 1 ]; then
  log "WARN refusing to rebuild the stock registry: a plugin bind of ours is still mounted, so the scan would not be reading stock plugins"
elif GST_REGISTRY_1_0="$CLEAN_REG" \
   GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0 \
   GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes \
   timeout 60 /usr/bin/gst-inspect-1.0 >/dev/null 2>>"$LOG" && [ -s "$CLEAN_REG" ]; then
  if cp -f "$CLEAN_REG" "$REG_TARGET" 2>>"$LOG"; then
    REG_OK=1
    log "regenerated clean stock registry (reverted cp-based override)"
  else
    log "WARN could not write the clean registry to $REG_TARGET"
  fi
else
  log "WARN clean registry regen failed (scan timed out, errored, or produced nothing); leaving existing registry untouched"
fi
rm -f "$CLEAN_REG" 2>/dev/null

# --- 3. Remove install dirs (ONLY once LG's registry is back) --------------
if [ "$REG_OK" = 1 ]; then
  if [ -f "$STOCK_FP" ]; then
    rm -f "$STOCK_FP" && log "removed $STOCK_FP"
  else
    log "$STOCK_FP not present"
  fi
  for D in "$DTS_DEST" "$THD_DEST" "$DMX_DEST"; do
    if [ -d "$D" ]; then
      rm -rf "$D" && log "removed $D"
    else
      log "$D not present"
    fi
  done
else
  log "DEFERRED: kept $DTS_DEST, $THD_DEST and $DMX_DEST because LG's registry could not be rebuilt"
fi

# --- 4. Restart media pipeline ---------------------------------------------
if killall starfish-media-pipeline 2>>"$LOG"; then
  log "restarted starfish-media-pipeline"
else
  log "note: starfish-media-pipeline not running"
fi

log "=== unified DTS+TrueHD uninstall done ==="
if [ "$REG_OK" = 1 ]; then
  echo "DTS + TrueHD uninstall complete (reboot recommended). See $LOG for details."
else
  echo ""
  echo "*** DTS + TrueHD uninstall INCOMPLETE ***"
  echo ""
  echo "The bind-mounts are gone and the boot hook is removed, so nothing of ours"
  echo "will be applied again. But rebuilding LG's plugin registry FAILED, so the"
  echo "staged files were deliberately KEPT -- which also means the live registry"
  echo "still lists our dtsdec and it can still load, so DTS may keep decoding"
  echo "until that registry is rebuilt:"
  echo "    $DTS_DEST"
  echo "    $THD_DEST"
  echo "    $DMX_DEST"
  echo "Deleting them while the live registry still references them would break"
  echo "audio in other apps. Please re-run:  sh uninstall.sh"
  echo "See $LOG for details."
fi
# Always 0: a non-zero exit here would trip the webosbrew failsafe.
exit 0
