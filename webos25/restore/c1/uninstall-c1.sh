#!/bin/sh
# uninstall-c1.sh — remove profile webos6-o20n-gst116-spdif completely.
# LG's registry file was never written (only bind-mounted over), so dropping
# the bind restores it exactly; nothing else outside our folders was touched.
set -u
STATE=/var/lib/webosbrew/dtsc1
HOOK=/var/lib/webosbrew/init.d/restore_dts_c1

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
rm -f "$HOOK"
[ -f "$STATE/init_dts_c1.sh" ] && sh "$STATE/init_dts_c1.sh" stop
# Belt and braces: no bind of ours may survive.
if [ -f "$STATE/env.conf" ]; then
  . "$STATE/env.conf"
  R=$(readlink -f "${REG:-/nonexistent}" 2>/dev/null)
  i=0
  while [ $i -lt 5 ] && { grep -q " ${REG:-/nonexistent} " /proc/mounts || grep -q " ${R:-/nonexistent} " /proc/mounts; }; do
    umount -n "$REG" 2>/dev/null || umount -n "$R" 2>/dev/null || break
    i=$((i + 1))
  done
fi
rm -rf "$STATE" /run/dtsc1
echo "Removed. LG's plugin registry is live again; start a new playback."
