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
#   - libgstdtsdec.so   the patched decoder plugin (armel soft-float), with
#                       FFmpeg n4.4.4's dca decoder linked in statically
#
# The decoder is FFmpeg's dca (core + DTS-HD XLL/XBR/X96/XXCH + LBR), not
# libdca (core only). It is built here as a static libavcodec/libavutil with
# every other codec disabled and linked with --exclude-libs,ALL, so none of
# its symbols are exported: libgstlibav loads its own shared libavcodec
# (build-truehd.sh) into the same media process, and the two must not bind to
# each other's functions.
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
OUT="${OUT:-$HERE/out}"
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

    # Single suite for both architectures. bookworm-security carries an arm64
    # libpcre2-8-0 10.42-1+deb12u1 that armel never got, and Multi-Arch: same
    # packages must match version for version, so with the security suite
    # enabled `libgstreamer1.0-dev:armel` is unsatisfiable: libglib2.0-0:armel,
    # libpcre2-dev:armel, libpcre2-posix3:armel and libselinux1:armel all report
    # "libpcre2-8-0:armel ... not going to be installed".
    # The suite is a pinned snapshot, and the image's own packages are moved to
    # it: newer debian:12-slim images already ship security-updated arm64 libs
    # (liblzma5, libpcre2, libselinux1) that no armel counterpart matches.
    SNAPSHOT=20250601T000000Z
    rm -f /etc/apt/sources.list.d/debian.sources
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s bookworm main\n' \
      "$SNAPSHOT" > /etc/apt/sources.list
    printf 'Package: *\nPin: origin "snapshot.debian.org"\nPin-Priority: 1001\n' \
      > /etc/apt/preferences.d/snapshot

    # Enable the armel (32-bit soft-float ARM) foreign architecture.
    dpkg --add-architecture armel
    apt-get update -qq >/dev/null 2>&1
    apt-get -y -qq --allow-downgrades dist-upgrade >/dev/null 2>&1

    # Cross toolchain + helpers (host arch: arm64).
    apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi pkg-config file patchelf binutils \
      git ca-certificates make >/dev/null 2>&1

    # armel dev packages: GStreamer core, plugins-base (audio/base libs), glib.
    apt-get install -y -qq --no-install-recommends \
      libgstreamer1.0-dev:armel \
      libgstreamer-plugins-base1.0-dev:armel \
      libglib2.0-dev:armel >/dev/null 2>&1

    # Static FFmpeg with only the dca decoder. Same tag and the same no-asm
    # soft-float settings as build-truehd.sh's shared build.
    git clone -q --depth 1 -b n4.4.4 https://git.ffmpeg.org/ffmpeg.git /tmp/ffmpeg
    ( cd /tmp/ffmpeg && ./configure --cross-prefix=arm-linux-gnueabi- \
        --enable-cross-compile --arch=arm --target-os=linux --prefix=/tmp/ffdca \
        --disable-everything --enable-decoder=dca \
        --disable-avformat --disable-avfilter --disable-swresample \
        --disable-swscale --disable-avdevice --disable-postproc \
        --disable-network --disable-programs --disable-doc --disable-autodetect \
        --disable-pthreads --disable-asm \
        --enable-static --disable-shared --enable-pic >/dev/null \
      && make -j"$(nproc)" install >/dev/null )
    grep -q "CONFIG_DCA_DECODER 1" /tmp/ffmpeg/config.h \
      || { echo "ERROR: FFmpeg configured without the dca decoder"; exit 1; }

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
    #   -shared -fPIC -O2 : a normal optimized shared plugin.
    #   VERSION / PACKAGE / GST_PACKAGE_* : plugin identity metadata.
    #   libavcodec.a libavutil.a + --exclude-libs,ALL : the static decoder,
    #        with none of its symbols exported (see the header comment).
    arm-linux-gnueabi-gcc -shared -fPIC -O2 -Wall -o /out/libgstdtsdec.so gstdtsdec.c \
      -I/tmp/ffdca/include \
      -DVERSION='"1.22.0-webosdts"' \
      -DPACKAGE='"gst-plugins-bad"' \
      -DGST_PACKAGE_NAME='"WebOS DTS restore"' \
      -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore"' \
      $CF /tmp/ffdca/lib/libavcodec.a /tmp/ffdca/lib/libavutil.a $LB -lm \
      -Wl,--exclude-libs,ALL -Wl,--no-undefined

    echo "=== BUILT ==="
    file -b /out/libgstdtsdec.so | cut -d, -f1-4
    echo -n "e_flags: "; od -An -tx4 -j36 -N4 /out/libgstdtsdec.so
    echo "=== NEEDED ==="
    readelf -d /out/libgstdtsdec.so | grep -E "NEEDED|RUNPATH|RPATH" | grep -oE "\[.*\]"
    echo "=== max GLIBC (must be <= 2.35) ==="
    objdump -T /out/libgstdtsdec.so 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -V | tail -1
    echo "=== exported FFmpeg symbols (must be 0) ==="
    n=$(objdump -T /out/libgstdtsdec.so | grep -cE " (av|ff|avpriv|avcodec|swr)_" || true)
    echo "$n"; [ "$n" = 0 ] || { echo "ERROR: FFmpeg symbols leak from the plugin"; exit 1; }
CONTAINER_EOF

echo ""
echo "=== DONE ==="
echo "Artifacts in $OUT:"
ls -la "$OUT"
