#!/bin/sh
# install-c1.sh — install profile webos6-o20n-gst116-spdif (LG OLED C1/G1).
# Run tv-test.sh first. Fully reversible with uninstall-c1.sh.
#
# Usage (root, from the folder holding libgstdtsdec.so + init_dts_c1.sh):
#   sh install-c1.sh                    decode only (stereo, TV speakers)
#   sh install-c1.sh --passthrough      also send DTS to an optical/ARC
#                                       receiver when it is the live output
#   REG=/path/to/registry.bin sh install-c1.sh ...   if detection is ambiguous
set -u
D=$(cd "$(dirname "$0")" && pwd)
STATE=/var/lib/webosbrew/dtsc1
HOOK=/var/lib/webosbrew/init.d/restore_dts_c1

MODE=${1:-}
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
for f in libgstdtsdec.so init_dts_c1.sh; do
  [ -f "$D/$f" ] || { echo "missing $D/$f"; exit 1; }
done

# LG's media registry: the one the media pipeline reads. Taken from the shell
# environment if set (Homebrew Channel exec/SSH sessions export it), else the
# single existing candidate. Never guessed between several.
REG=${REG:-${GST_REGISTRY_1_0:-}}
if [ -z "$REG" ]; then
  set -- $(ls /mnt/flash/data/gst_1_0_registry*.bin /var/cache/gstreamer-1.0/registry*.bin 2>/dev/null)
  if [ "$#" -eq 1 ]; then REG=$1; else
    echo "Could not pick LG's media registry automatically. Candidates: ${*:-none}"
    echo "Re-run with REG=<path> sh $0 $MODE"
    exit 1
  fi
fi
[ -f "$REG" ] || { echo "registry $REG does not exist"; exit 1; }
echo "LG media registry: $REG"

mkdir -p "$STATE/plugin" /var/lib/webosbrew/init.d
# Stop a previous install of ours first (drops our bind, keeps LG's file).
[ -x "$STATE/init_dts_c1.sh" ] && sh "$STATE/init_dts_c1.sh" stop
cp -f "$D/libgstdtsdec.so" "$STATE/plugin/libgstdtsdec.so"
cp -f "$D/init_dts_c1.sh" "$STATE/init_dts_c1.sh"
chmod 755 "$STATE/init_dts_c1.sh"
chmod 644 "$STATE/plugin/libgstdtsdec.so"
echo "REG=$REG" > "$STATE/env.conf"
case "$MODE" in
  --passthrough) : > "$STATE/passthrough"; echo "passthrough: ON (receiver on optical/ARC + Pass Through)" ;;
  *) rm -f "$STATE/passthrough"; echo "passthrough: off (decode only)" ;;
esac
ln -sf "$STATE/init_dts_c1.sh" "$HOOK"

echo "--- applying"
sh "$STATE/init_dts_c1.sh"
sh "$STATE/init_dts_c1.sh" status
echo
echo "Done. Start a NEW playback (close the player first). To undo everything:"
echo "  sh $D/uninstall-c1.sh"
