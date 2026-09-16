#!/usr/bin/env bash
#
# build-switch.sh — Cross-build the `adecswitch` GStreamer plugin for webOS 25
# (LG C5). Sibling of build.sh; same target ABI, same container recipe.
#
# Target ABI (verified on-device for the other payload binaries):
#   32-bit ARM, EABI5 *soft-float* (ld-linux.so.3), glibc 2.35, GStreamer 1.24.
#   That is Debian's `armel` port, so we cross-compile with
#   `arm-linux-gnueabi-gcc` (soft-float) — NOT `arm-linux-gnueabihf`.
#
# Produces in webos25/restore/switch-out/:
#   - libgstadecswitch.so   the decoder-switching bin (armel soft-float)
#
# adecswitch links nothing but GStreamer core + glib (no libdca, no ffmpeg):
# it only instantiates other elements by factory name.
#
# Requirements on the build host: Docker (or podman aliased to docker) with
# qemu/binfmt for linux/arm64.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/switch-out"
GLIBC_CEILING=2.35
mkdir -p "$OUT"

echo "=== adecswitch webOS25 cross-build ==="
echo "src: $HERE/src"
echo "out: $OUT"

docker run --rm -i --platform linux/arm64 \
  -v "$HERE/src":/work:ro \
  -v "$OUT":/out \
  -e GLIBC_CEILING="$GLIBC_CEILING" \
  debian:12-slim /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
    export DEBIAN_FRONTEND=noninteractive

    # Single suite for both architectures. bookworm-security carries an arm64
    # libpcre2-8-0 10.42-1+deb12u1 that armel never got, and Multi-Arch: same
    # packages must match version for version, so with the security suite
    # enabled `libgstreamer1.0-dev:armel` is unsatisfiable ("libpcre2-8-0:armel
    # ... not going to be installed").
    rm -f /etc/apt/sources.list.d/debian.sources
    printf 'deb http://deb.debian.org/debian bookworm main\n' > /etc/apt/sources.list

    dpkg --add-architecture armel
    apt-get update -qq >/dev/null 2>&1

    apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi pkg-config file binutils >/dev/null 2>&1

    # armel dev packages: GStreamer core + glib. Nothing else is needed.
    apt-get install -y -qq --no-install-recommends \
      libgstreamer1.0-dev:armel \
      libglib2.0-dev:armel >/dev/null 2>&1

    cp /work/gstadecswitch.c /work/gstadecswitch.h /tmp/
    cd /tmp

    export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig
    CF=$(pkg-config --cflags gstreamer-1.0)
    LB=$(pkg-config --libs   gstreamer-1.0)

    arm-linux-gnueabi-gcc -shared -fPIC -O2 -Wall -o /out/libgstadecswitch.so \
      gstadecswitch.c \
      -DVERSION='"1.24.0-webosdts"' \
      -DPACKAGE='"dts_restore_plus"' \
      -DGST_PACKAGE_NAME='"WebOS DTS restore"' \
      -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore"' \
      $CF $LB

    arm-linux-gnueabi-strip --strip-unneeded /out/libgstadecswitch.so

    SO=/out/libgstadecswitch.so

    echo "=== BUILT ==="
    file -b "$SO" | cut -d, -f1-4

    echo "=== readelf -h ==="
    arm-linux-gnueabi-readelf -h "$SO"

    # Soft-float assertion: an armhf build would say "hard-float ABI" here and
    # would silently fail to load on the TV.
    FLAGS=$(arm-linux-gnueabi-readelf -h "$SO" | grep -E '^\s*Flags:')
    echo "$FLAGS" | grep -q "soft-float ABI" \
      || { echo "ERROR: not a soft-float build: $FLAGS"; exit 1; }
    echo "$FLAGS" | grep -q "Version5 EABI" \
      || { echo "ERROR: not EABI5: $FLAGS"; exit 1; }
    echo -n "e_flags: "; od -An -tx4 -j36 -N4 "$SO"

    echo "=== NEEDED ==="
    arm-linux-gnueabi-readelf -d "$SO" | grep -E "NEEDED|RUNPATH|RPATH" \
      | grep -oE "\[.*\]"

    echo "=== GLIBC symbol versions (ceiling $GLIBC_CEILING) ==="
    arm-linux-gnueabi-readelf -Ws "$SO" | grep -oE "GLIBC_[0-9.]+" \
      | sort -uV || true
    MAXG=$(arm-linux-gnueabi-readelf -Ws "$SO" | grep -oE "GLIBC_[0-9.]+" \
      | sed 's/GLIBC_//' | sort -uV | tail -1)
    echo "max GLIBC: ${MAXG:-none}"
    if [ -n "${MAXG:-}" ]; then
      HIGH=$(printf '%s\n%s\n' "$MAXG" "$GLIBC_CEILING" | sort -V | tail -1)
      [ "$HIGH" = "$GLIBC_CEILING" ] \
        || { echo "ERROR: needs GLIBC_$MAXG > ceiling $GLIBC_CEILING"; exit 1; }
    fi

    # Registration check: run the armel gst-inspect-1.0 under qemu-user so the
    # rank / klass / pad templates are read out of the real binary rather than
    # assumed. Non-fatal: if the armel tools or qemu are unavailable the build
    # still succeeds and says so.
    echo "=== gst-inspect (armel under qemu-user) ==="
    if apt-get install -y -qq --no-install-recommends \
         qemu-user-static gstreamer1.0-tools:armel >/dev/null 2>&1; then
      export GST_REGISTRY_1_0=/tmp/adecswitch-registry.bin
      export GST_REGISTRY_FORK=no
      export GST_PLUGIN_PATH=/out
      if qemu-arm-static /usr/bin/gst-inspect-1.0 adecswitch; then
        :
      else
        echo "SKIPPED: gst-inspect-1.0 could not run under qemu-user"
      fi
    else
      echo "SKIPPED: armel gstreamer1.0-tools / qemu-user-static unavailable"
    fi
CONTAINER_EOF

echo ""
echo "=== BUILD-REPORT ==="
echo "artifact : $OUT/libgstadecswitch.so"
echo "source   : $HERE/src/gstadecswitch.c"
echo "target   : armel (ARM EABI5 soft-float), glibc <= $GLIBC_CEILING, GStreamer 1.24"
echo "element  : adecswitch, rank 320, klass Codec/Decoder/Audio"
echo ""
echo "NOT VERIFIED BY THIS SCRIPT: on-device behaviour. Per"
echo ".claude/rules/releasing.md the committed .so must be verified on a real"
echo "webOS-25 TV before release."
ls -la "$OUT"
