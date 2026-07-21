#!/bin/bash
# DTS playback restoration uninstaller for LG webOS TVs.
# Copyright (c) 2022-2025 Pete Batard <pete@akeo.ie>
# See https://github.com/RootMyTV/RootMyTV.github.io/issues/72
set -u

PAYLOAD_DIR=/var/lib/webosbrew/dts_restore
INIT_DIR=/var/lib/webosbrew/init.d
INIT_STUB=$INIT_DIR/restore_dts

if [ "$(id -u)" != "0" ]; then
  echo "This uninstaller must be run as root - Aborting"
  exit 1
fi

removed=0

# Remove the boot stub (also clears a dangling symlink left by older versions).
if [ -e "$INIT_STUB" ] || [ -L "$INIT_STUB" ]; then
  rm -f "$INIT_STUB"
  removed=1
  echo "Removed $INIT_STUB"
fi

# Unmount the live library overrides (best-effort).
for lib in libgstisomp4.so libgstisomp4_1_8.so libgstmatroska.so libgstlibav.so libgstmpegtsdemux.so; do
  target="/usr/lib/gstreamer-1.0/$lib"
  if grep -q " $target " /proc/mounts 2>/dev/null; then
    umount "$target" 2>/dev/null && echo "Unmounted $target"
  fi
done

# Unmount the gstcool.conf override and drop the generated copy.
if grep -q " /etc/gst/gstcool.conf " /proc/mounts 2>/dev/null; then
  umount /etc/gst/gstcool.conf 2>/dev/null && echo "Unmounted gstcool.conf override"
fi
rm -f /tmp/gstcool.conf

# Unmount the GStreamer registry override, using the path baked at install time.
if [ -f "$PAYLOAD_DIR/env.conf" ]; then
  . "$PAYLOAD_DIR/env.conf"
  if [ -n "${BAKED_GST_REGISTRY:-}" ] && grep -q " $BAKED_GST_REGISTRY " /proc/mounts 2>/dev/null; then
    umount "$BAKED_GST_REGISTRY" 2>/dev/null && echo "Unmounted registry override"
  fi
fi

# Remove the installed payload.
if [ -d "$PAYLOAD_DIR" ]; then
  rm -rf "$PAYLOAD_DIR"
  removed=1
  echo "Removed $PAYLOAD_DIR"
fi

if [ "$removed" -eq 0 ]; then
  echo "Nothing to uninstall."
  exit 0
fi

echo
echo "DTS playback has been uninstalled."
echo "Fully power off the TV to clear the GStreamer registry override. If Quick"
echo "Start+ is enabled the TV never truly powers off - unplug it, or disable"
echo "Settings > General > Quick Start+ and then power-cycle."
