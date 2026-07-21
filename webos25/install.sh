#!/bin/sh
#
# install.sh — Install the patched dtsdec DTS decoder on a rooted LG webOS 25 TV.
#
# Run as root ON THE TV, from a directory that also contains ./out/ with the
# cross-built artifacts (libgstdtsdec.so, libdca.so.0). Copy this whole webos25/
# folder to the TV (e.g. via scp) and run: sh install.sh
#
# What it does (all idempotent, guarded, logged to /tmp/dts25.log):
#   1. Installs libgstdtsdec.so -> /var/lib/webosbrew/dts25/
#      and libdca.so.0        -> /var/lib/webosbrew/dts25/libs/
#   2. Writes /var/lib/webosbrew/dts25/init_dts25.sh which regenerates the
#      media GStreamer registry (including dtsdec) into /tmp and bind-mounts it
#      over the media registry the starfish media pipeline reads.
#   3. Symlinks that into /var/lib/webosbrew/init.d/restore_dts25 (boot hook).
#   4. Applies it now and restarts the media pipeline.
#
# Always exits 0 so it is safe as a boot hook.
#
set -u

LOG=/tmp/dts25.log
DEST=/var/lib/webosbrew/dts25
LIBS=$DEST/libs
INITD=/var/lib/webosbrew/init.d
HOOK=$INITD/restore_dts25
INIT_SCRIPT=$DEST/init_dts25.sh
REG_TARGET=/mnt/flash/data/gst_1_0_registry.arm.bin

# Directory this script lives in (so it finds ./out/ regardless of cwd).
SELF_DIR=$(cd "$(dirname "$0")" && pwd)

log() { echo "[dts25-install $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

log "=== install start ==="

# --- 1. Install binaries ---------------------------------------------------
mkdir -p "$LIBS" || { log "FATAL: cannot create $LIBS"; exit 0; }

if [ -f "$SELF_DIR/out/libgstdtsdec.so" ]; then
  cp -f "$SELF_DIR/out/libgstdtsdec.so" "$DEST/libgstdtsdec.so" \
    && log "installed libgstdtsdec.so -> $DEST/" \
    || log "WARN: failed to copy libgstdtsdec.so"
else
  log "WARN: $SELF_DIR/out/libgstdtsdec.so not found (build it first)"
fi

if [ -f "$SELF_DIR/out/libdca.so.0" ]; then
  cp -f "$SELF_DIR/out/libdca.so.0" "$LIBS/libdca.so.0" \
    && log "installed libdca.so.0 -> $LIBS/" \
    || log "WARN: failed to copy libdca.so.0"
else
  log "WARN: $SELF_DIR/out/libdca.so.0 not found (build it first)"
fi

# --- 2. Write the runtime init script (registry regen + bind-mount) --------
# This is what actually injects dtsdec into the media pipeline's registry.
# It regenerates a fresh registry that includes dtsdec (by pointing the plugin
# path at our dir plus the stock plugin dirs), then bind-mounts the result over
# the registry file the media process trusts. GST_REGISTRY_UPDATE=yes on the
# regen; the media process itself uses UPDATE=no and simply reads our file.
cat > "$INIT_SCRIPT" <<'INITEOF'
#!/bin/sh
# init_dts25.sh — regenerate + bind-mount the media GStreamer registry so that
# the patched dtsdec is available to starfish-media-pipeline. Idempotent.
set -u

LOG=/tmp/dts25.log
DEST=/var/lib/webosbrew/dts25
REG_TMP=/tmp/gst_dts_reg.bin
REG_TARGET=/mnt/flash/data/gst_1_0_registry.arm.bin

log() { echo "[dts25-init $(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>&1; }

log "init: regenerating registry"

# Regenerate a registry that includes our plugin. Search stock plugin dirs plus
# our dts25 dir. gst-inspect-1.0 with GST_REGISTRY_UPDATE=yes writes REG_TMP.
GST_REGISTRY_1_0="$REG_TMP" \
GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:"$DEST" \
GST_REGISTRY_UPDATE=yes \
  gst-inspect-1.0 >/dev/null 2>>"$LOG"

if [ ! -f "$REG_TMP" ]; then
  log "init: ERROR registry regen produced no $REG_TMP"
  exit 0
fi

# Confirm dtsdec made it into the regenerated registry (best-effort check).
if GST_REGISTRY_1_0="$REG_TMP" GST_REGISTRY_UPDATE=no \
     GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:"$DEST" \
     gst-inspect-1.0 dtsdec >/dev/null 2>&1; then
  log "init: dtsdec present in regenerated registry"
else
  log "init: WARN dtsdec not confirmed in regenerated registry"
fi

# Bind-mount our registry over the one the media pipeline reads. If already
# mounted, refresh by re-mounting (unmount first, ignore errors).
if mount | grep -q " $REG_TARGET "; then
  umount "$REG_TARGET" 2>>"$LOG" || log "init: WARN could not umount existing bind"
fi

if mount --bind "$REG_TMP" "$REG_TARGET" 2>>"$LOG"; then
  log "init: bind-mounted $REG_TMP over $REG_TARGET"
else
  log "init: ERROR bind-mount failed"
fi
INITEOF
chmod 0755 "$INIT_SCRIPT" && log "wrote $INIT_SCRIPT"

# --- 3. Boot hook symlink --------------------------------------------------
mkdir -p "$INITD"
if [ -L "$HOOK" ] || [ -e "$HOOK" ]; then
  rm -f "$HOOK"
fi
ln -s "$INIT_SCRIPT" "$HOOK" && log "linked boot hook $HOOK -> $INIT_SCRIPT"

# --- 4. Apply now ----------------------------------------------------------
log "applying now"
sh "$INIT_SCRIPT"

# Restart the media pipeline so it picks up the new registry.
if killall starfish-media-pipeline 2>>"$LOG"; then
  log "restarted starfish-media-pipeline"
else
  log "note: starfish-media-pipeline not running (will start fresh on next playback)"
fi

log "=== install done ==="
echo "dts25 install complete. See $LOG for details."
exit 0
