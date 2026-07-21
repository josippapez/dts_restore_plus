#!/bin/sh
#
# uninstall.sh — Remove the webOS 25 dts25 DTS-restore install.
#
# Run as root ON THE TV: sh uninstall.sh
#
# Reverses install.sh:
#   1. Removes the boot hook symlink.
#   2. Unmounts the registry bind-mount (reverting to LG's original registry).
#   3. Removes /var/lib/webosbrew/dts25.
#   4. Restarts the media pipeline.
#
# Always exits 0.
#
set -u

LOG=/tmp/dts25.log
DEST=/var/lib/webosbrew/dts25
HOOK=/var/lib/webosbrew/init.d/restore_dts25
REG_TARGET=/mnt/flash/data/gst_1_0_registry.arm.bin
REG_TMP=/tmp/gst_dts_reg.bin

log() { echo "[dts25-uninstall $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

log "=== uninstall start ==="

# --- 1. Remove boot hook ---------------------------------------------------
if [ -L "$HOOK" ] || [ -e "$HOOK" ]; then
  rm -f "$HOOK" && log "removed boot hook $HOOK"
else
  log "boot hook not present"
fi

# --- 2. Unmount registry bind ----------------------------------------------
if mount | grep -q " $REG_TARGET "; then
  if umount "$REG_TARGET" 2>>"$LOG"; then
    log "unmounted registry bind over $REG_TARGET (reverted to LG original)"
  else
    log "WARN could not unmount $REG_TARGET"
  fi
else
  log "no registry bind-mount present"
fi
rm -f "$REG_TMP" 2>/dev/null

# --- 3. Remove install dir -------------------------------------------------
if [ -d "$DEST" ]; then
  rm -rf "$DEST" && log "removed $DEST"
else
  log "$DEST not present"
fi

# --- 4. Restart media pipeline ---------------------------------------------
if killall starfish-media-pipeline 2>>"$LOG"; then
  log "restarted starfish-media-pipeline"
else
  log "note: starfish-media-pipeline not running"
fi

log "=== uninstall done ==="
echo "dts25 uninstall complete. See $LOG for details."
exit 0
