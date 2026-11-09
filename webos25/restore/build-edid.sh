#!/usr/bin/env bash
#
# build-edid.sh — Cross-build libedidaudio.so, the LD_PRELOAD hook that keeps
# "Show DTS tracks in apps" out of the HDMI-input EDID (see src/edidaudio.c).
# Same target ABI and container recipe as build-switch.sh: 32-bit ARM, EABI5
# soft-float (ld-linux.so.3), glibc 2.35. It loads into LG's /usr/sbin/eim.
#
# Produces webos25/restore/edid-out/libedidaudio.so. Requires Docker.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/edid-out}"
GLIBC_CEILING=2.35
mkdir -p "$OUT"

docker run --rm -i \
  -v "$HERE/src":/work:ro \
  -v "$OUT":/out \
  -e GLIBC_CEILING="$GLIBC_CEILING" \
  debian:12-slim /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
    export DEBIAN_FRONTEND=noninteractive
    SNAPSHOT=20250601T000000Z
    rm -f /etc/apt/sources.list.d/debian.sources
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s bookworm main\n' "$SNAPSHOT" > /etc/apt/sources.list
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi libc6-dev-armel-cross binutils file >/dev/null 2>&1

    SO=/out/libedidaudio.so
    arm-linux-gnueabi-gcc -shared -fPIC -O2 -Wall -Wextra -Werror -std=gnu11 \
      -o "$SO" /work/edidaudio.c -Wl,--no-undefined
    arm-linux-gnueabi-strip --strip-unneeded "$SO"

    echo "=== BUILT ==="
    file -b "$SO" | cut -d, -f1-4
    FLAGS=$(arm-linux-gnueabi-readelf -h "$SO" | grep -E '^\s*Flags:')
    echo "$FLAGS" | grep -q "soft-float ABI" || { echo "ERROR: not a soft-float build: $FLAGS"; exit 1; }
    echo "$FLAGS" | grep -q "Version5 EABI" || { echo "ERROR: not EABI5: $FLAGS"; exit 1; }

    # The hook only works if it exports the library's exact mangled name.
    arm-linux-gnueabi-readelf -Ws --dyn-syms "$SO" \
      | grep -q " FUNC .* GLOBAL .* _Z23_EDID_CTRL_GetAudioType23EDID_AUDIO_INPUT_TYPE_Tb23EDID_AUDIO_CAPABILITY_Tbbbb$" \
      || { echo "ERROR: _EDID_CTRL_GetAudioType is not exported"; exit 1; }

    echo "=== NEEDED ==="
    arm-linux-gnueabi-readelf -d "$SO" | grep -E "NEEDED" | grep -oE "\[.*\]"
    MAXG=$(arm-linux-gnueabi-readelf -Ws "$SO" | grep -oE "GLIBC_[0-9.]+" | sed 's/GLIBC_//' | sort -uV | tail -1)
    echo "max GLIBC: ${MAXG:-none}"
    if [ -n "${MAXG:-}" ]; then
      HIGH=$(printf '%s\n%s\n' "$MAXG" "$GLIBC_CEILING" | sort -V | tail -1)
      [ "$HIGH" = "$GLIBC_CEILING" ] || { echo "ERROR: needs GLIBC_$MAXG > ceiling $GLIBC_CEILING"; exit 1; }
    fi
    echo "md5: $(md5sum "$SO" | cut -d' ' -f1)"
CONTAINER_EOF
