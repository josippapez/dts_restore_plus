#!/usr/bin/env bash
#
# build.sh — Cross-build the patched `dtsdec` GStreamer plugin for webOS 25 (LG C5).
#
# Target ABI (verified on-device):
#   32-bit ARM, EABI5 *soft-float* (ld-linux.so.3), glibc 2.35, GStreamer 1.24.
#   This matches Debian's `armel` port, so we cross-compile with
#   `arm-linux-gnueabi-gcc` (soft-float) — NOT `arm-linux-gnueabihf` (hard-float).
#
# What this produces in webos25/out/:
#   - libgstdtsdec.so   the patched decoder plugin (armel soft-float)
#   - libdca.so.0       the DTS decode library (armel), bundled for the TV
#
# The plugin's sink caps are already patched in src/gstdtsdec.c to accept LG's
# retagged raw DTS ("audio/x-unknown, codec-id=(string)A_DTS"), so this script
# does NOT modify the source — it only compiles it.
#
# Requirements on the build host: Docker with qemu/binfmt for linux/arm64
# (e.g. `docker run --privileged --rm tonistiigi/binfmt --install arm64`).
# We build inside debian:12-slim (bookworm) on the arm64 platform so that the
# armel cross-toolchain and :armel dev packages resolve cleanly.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
mkdir -p "$OUT"

echo "=== dtsdec webOS25 cross-build ==="
echo "src: $HERE/src"
echo "out: $OUT"

# Run the whole build inside an arm64 Debian 12 container. We bind-mount the
# vendored source read-only at /work and the output dir at /out.
docker run --rm -i --platform linux/arm64 \
  -v "$HERE/src":/work:ro \
  -v "$OUT":/out \
  debian:12-slim /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
    export DEBIAN_FRONTEND=noninteractive

    # Enable the armel (32-bit soft-float ARM) foreign architecture.
    dpkg --add-architecture armel
    apt-get update -qq >/dev/null 2>&1

    # Cross toolchain + helpers (host arch: arm64).
    apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi pkg-config file patchelf binutils >/dev/null 2>&1

    # armel dev packages: GStreamer core, plugins-base (audio/base libs),
    # libdca (the DTS decoder), and glib.
    apt-get install -y -qq --no-install-recommends \
      libgstreamer1.0-dev:armel \
      libgstreamer-plugins-base1.0-dev:armel \
      libdca-dev:armel \
      libglib2.0-dev:armel >/dev/null 2>&1

    # Work on a writable copy (source mount is read-only).
    cp /work/gstdtsdec.c /work/gstdtsdec.h /tmp/
    cd /tmp

    # Sanity: confirm the caps patch is already present in the vendored source.
    echo "--- caps line (must include A_DTS) ---"
    grep -n "x-unknown" gstdtsdec.c || { echo "ERROR: caps patch missing from source"; exit 1; }

    # Point pkg-config at the armel multiarch pkgconfig dirs.
    export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig
    CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)
    LB=$(pkg-config --libs   gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)

    # Compile. Key flags:
    #   -include stdint.h -include inttypes.h : libdca headers use int types
    #        without always including these; force-include avoids build breaks.
    #   -DHAVE_ORC=0 : no Orc SIMD runtime on the TV; disable that code path.
    #   -shared -fPIC -O2 : a normal optimized shared plugin.
    #   VERSION / PACKAGE / GST_PACKAGE_* : plugin identity metadata.
    #   -ldca : link the DTS decode library.
    #   -Wl,-rpath,... : bake the on-TV libs dir so libdca.so.0 is found there.
    arm-linux-gnueabi-gcc -shared -fPIC -O2 -o /out/libgstdtsdec.so gstdtsdec.c \
      -include stdint.h -include inttypes.h \
      -DHAVE_ORC=0 \
      -DVERSION='"1.22.0-webosdts"' \
      -DPACKAGE='"gst-plugins-bad"' \
      -DGST_PACKAGE_NAME='"WebOS DTS restore"' \
      -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore"' \
      $CF $LB -ldca \
      -Wl,-rpath,/var/lib/webosbrew/dts25/libs

    # Bundle the armel libdca.so.0 for deployment onto the TV.
    DCA_SO=$(find / -name "libdca.so.0*" -path "*arm-linux-gnueabi*" 2>/dev/null | head -1)
    if [ -z "$DCA_SO" ]; then
      echo "ERROR: could not locate armel libdca.so.0"; exit 1
    fi
    cp -L "$DCA_SO" /out/libdca.so.0

    echo "=== BUILT ==="
    file -b /out/libgstdtsdec.so | cut -d, -f1-4
    echo -n "e_flags: "; od -An -tx4 -j36 -N4 /out/libgstdtsdec.so
    echo "=== NEEDED ==="
    readelf -d /out/libgstdtsdec.so | grep -E "NEEDED|RUNPATH|RPATH" | grep -oE "\[.*\]"
    echo "=== max GLIBC (must be <= 2.35) ==="
    objdump -T /out/libgstdtsdec.so 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -V | tail -1
    echo "=== libdca ==="
    file -b /out/libdca.so.0 | cut -d, -f1-4
CONTAINER_EOF

echo ""
echo "=== DONE ==="
echo "Artifacts in $OUT:"
ls -la "$OUT"
