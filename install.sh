#!/bin/bash
# DTS playback restoration installer for LG webOS TVs (OLED CX and compatible).
# Copyright (c) 2022-2025 Pete Batard <pete@akeo.ie>
# See https://github.com/RootMyTV/RootMyTV.github.io/issues/72#issuecomment-1343204028
set -u

# --- Locate this script's directory (works on busybox ash and webOS bash) ---
LAST_COMMAND="$_"  # IMPORTANT: this must be the first command in the script
ps_output="$(ps -o pid,comm | grep -Fw $$)"
for cs in $ps_output; do
  CURRENT_SHELL=$cs
done
if [ -n "${BASH_SOURCE:-}" ]; then
  SCRIPT="${BASH_SOURCE[0]}"
elif [ "$0" != "$CURRENT_SHELL" ] && [ "$0" != "-$CURRENT_SHELL" ]; then
  SCRIPT="$0"
elif [ -n "$LAST_COMMAND" ]; then
  SCRIPT="$LAST_COMMAND"
else
  echo "Could not get script path - Aborting"
  exit 1
fi
SCRIPT=$(realpath "$SCRIPT" 2>&-)
SCRIPT_DIR=$(dirname "$SCRIPT")
GST_SRC=$SCRIPT_DIR/gst

PAYLOAD_DIR=/var/lib/webosbrew/dts_restore
INIT_DIR=/var/lib/webosbrew/init.d
INIT_STUB=$INIT_DIR/restore_dts   # must NOT end in .sh (run-parts skips dotted names)

# --- Options ---
ASSUME_YES=0
for arg in "$@"; do
  case "$arg" in
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help) echo "Usage: install.sh [-y|--yes]"; exit 0 ;;
    *) echo "Unknown option: $arg"; exit 1 ;;
  esac
done

# --- Preflight ---
if [ "$(id -u)" != "0" ]; then
  echo "This installer must be run as root - Aborting"
  exit 1
fi
for tool in gst-inspect-1.0 nyx-cmd mount realpath; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required tool '$tool' not found - Aborting"
    exit 1
  fi
done
if [ ! -d "$INIT_DIR" ]; then
  echo "$INIT_DIR/ is missing - is webosbrew installed? - Aborting"
  exit 1
fi
if [ -z "${GST_REGISTRY_1_0:-}" ] || [ ! -f "$GST_REGISTRY_1_0" ]; then
  echo "Could not locate the GStreamer registry on this environment - Aborting"
  echo
  echo "Please use an ssh session (not telnet) to run this installer: telnet is"
  echo "missing the required environment variables and is known to cause this."
  exit 1
fi
# The media player must be closed. The [s]tarfish trick stops grep matching itself,
# so ANY count greater than zero means a real media-player instance is running.
if [ "$(ps -ef | grep -c '[s]tarfish-media')" -gt 0 ]; then
  echo "Cannot install while the media player is running - close any video and retry - Aborting"
  exit 1
fi
for lib in libgstmatroska.so libgstlibav.so; do
  if [ ! -f "$GST_SRC/$lib" ]; then
    echo "$GST_SRC/$lib is missing - Aborting"
    exit 1
  fi
done

# --- Platform check (community-confirmed allowlist) ---
# The shipped binaries are GStreamer 1.14.4 / OLED CX builds. They are field-confirmed
# on other 1.14-class LG sets (issue #72): BX, C1/G1, C2/G2 and 2020-2022 NanoCell/LCD.
# There is only ONE library set - the allowlist just suppresses the off-target warning;
# it never selects different binaries.
GST_VERSION=$(gst-inspect-1.0 --version | grep GStreamer | cut -d " " -f 2)
WEBOS_VERSION=$(nyx-cmd OSInfo query webos_release)
MODEL_NAME=$(nyx-cmd DeviceInfo query product_id)
SUPPORTED=0
case "$MODEL_NAME" in
  OLED*CX*|OLED*BX*|OLED*C1*|OLED*G1*|OLED*C2*|OLED*G2*|*UN7*|*NANO7*) SUPPORTED=1 ;;
esac
if [ "$SUPPORTED" -ne 1 ] || [ "$GST_VERSION" != "1.14.4" ]; then
  echo
  echo "This installer targets LG OLED CX (webOS 5.x, GStreamer 1.14.4) and is"
  echo "community-confirmed on BX, C1/G1, C2/G2 and 2020-2022 NanoCell/LCD sets."
  echo "You are on a(n) $MODEL_NAME TV with webOS $WEBOS_VERSION and GStreamer $GST_VERSION,"
  echo "which is not on that list. The SAME CX libraries will be used regardless."
  echo
  echo "Installing on an unlisted platform should not cause irreversible damage, but:"
  echo "1. The software may not work as expected, if at all."
  echo "2. You may lose existing features and/or functionality."
  echo "3. The entire responsibility lies with you."
  echo
  if [ "$ASSUME_YES" -ne 1 ]; then
    if [ ! -t 0 ]; then
      echo "Non-interactive run on an unlisted platform without --yes - Aborting"
      exit 1
    fi
    read -r -p "Do you wish to proceed? [y/N] " response
    case "$response" in
      [yY][eE][sS]|[yY]) ;;
      *) exit 1 ;;
    esac
  fi
fi

# --- Install the payload into a self-owned dir ---
# (robust: the overrides no longer depend on where the tarball was extracted, so
# deleting or moving the download does not silently break DTS at the next boot).
echo "Installing payload to $PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR/gst" || { echo "Could not create $PAYLOAD_DIR - Aborting"; exit 1; }
cp -f "$GST_SRC"/*.so "$PAYLOAD_DIR/gst/" || { echo "Could not copy libraries - Aborting"; exit 1; }
cp -f "$SCRIPT_DIR/init_dts.sh" "$PAYLOAD_DIR/init_dts.sh" || { echo "Could not copy init_dts.sh - Aborting"; exit 1; }
chmod 755 "$PAYLOAD_DIR/init_dts.sh"

# User-editable downmix coefficients, preserved across reinstalls.
if [ ! -f "$PAYLOAD_DIR/downmix.conf" ]; then
  cp -f "$SCRIPT_DIR/downmix.conf" "$PAYLOAD_DIR/downmix.conf" 2>/dev/null || {
    printf 'front=1.25\ncenter=0.75\nlfe=0.75\nrear=0.75\nrear2=0.70\n' > "$PAYLOAD_DIR/downmix.conf"
  }
fi

# Bake the GStreamer registry path captured from this (ssh) session.
cat > "$PAYLOAD_DIR/env.conf" <<EOT
# Captured by install.sh at install time - boot has no GST env of its own.
BAKED_GST_REGISTRY="$GST_REGISTRY_1_0"
EOT

# Clear any stale kill-switch left by a previous "disable".
rm -f "$PAYLOAD_DIR/disabled"

# --- Install the boot stub (idempotent) ---
echo "Installing $INIT_STUB"
cat > "$INIT_STUB" <<EOT
#!/bin/sh
exec $PAYLOAD_DIR/init_dts.sh
EOT
chmod 755 "$INIT_STUB"

# --- Apply immediately (no reboot required) ---
echo "Applying overrides now..."
"$PAYLOAD_DIR/init_dts.sh"
echo
echo "DTS and MKV DV playback have been enabled - Enjoy!"
echo "  log:     /tmp/dts_restore.log"
echo "  downmix: $PAYLOAD_DIR/downmix.conf"
