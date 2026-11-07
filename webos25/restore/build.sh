#!/usr/bin/env bash
#
# build.sh — Cross-build the patched `dtsdec` GStreamer plugin for webOS 25 (LG C5).
#
# Target ABI (verified on-device):
#   32-bit ARM, EABI5 *soft-float* (ld-linux.so.3), glibc 2.35, GStreamer 1.24.
#   This matches Debian's `armel` port, so we cross-compile with
#   `arm-linux-gnueabi-gcc` (soft-float) — NOT `arm-linux-gnueabihf` (hard-float).
#
# What this produces in webos25/restore/out/:
#   - libgstdtsdec.so   the patched decoder plugin (armel soft-float), with
#                       FFmpeg n4.4.4's dca decoder and spdif muxer linked in
#                       statically
#   - lgroute-control  named ALSA mixer validation/recovery helper
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
# Requirements on the build host: Docker. The build runs natively on the host
# (no qemu): the armel cross-toolchain is an ordinary amd64/arm64 package.
# The toolchain + static FFmpeg live in a cached image; Docker's layer
# cache rebuilds it only when the recipe below changes, so a source-only
# rebuild compiles just gstdtsdec.c.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/out}"
mkdir -p "$OUT"

docker build -t dts25-dtsdec-build - <<'DOCKERFILE_EOF'
FROM debian:12-slim
ENV DEBIAN_FRONTEND=noninteractive

# Single suite for both architectures. bookworm-security carries a host
# libpcre2-8-0 10.42-1+deb12u1 that armel never got, and Multi-Arch: same
# packages must match version for version, so with the security suite
# enabled `libgstreamer1.0-dev:armel` is unsatisfiable: libglib2.0-0:armel,
# libpcre2-dev:armel, libpcre2-posix3:armel and libselinux1:armel all report
# "libpcre2-8-0:armel ... not going to be installed".
# The suite is a pinned snapshot, and the image's own packages are moved to
# it: newer debian:12-slim images already ship security-updated host libs
# (liblzma5, libpcre2, libselinux1) that no armel counterpart matches.
RUN SNAPSHOT=20250601T000000Z \
 && rm -f /etc/apt/sources.list.d/debian.sources \
 && printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s bookworm main\n' \
      "$SNAPSHOT" > /etc/apt/sources.list \
 && printf 'Package: *\nPin: origin "snapshot.debian.org"\nPin-Priority: 1001\n' \
      > /etc/apt/preferences.d/snapshot \
 && dpkg --add-architecture armel \
 && apt-get update -qq \
 && apt-get -y -qq --allow-downgrades dist-upgrade \
 && apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi pkg-config git ca-certificates make \
      libgstreamer1.0-dev:armel \
      libgstreamer-plugins-base1.0-dev:armel \
      libglib2.0-dev:armel \
 && rm -rf /var/lib/apt/lists/*

# Static FFmpeg with only the dca decoder. Same tag and the same no-asm
# soft-float settings as build-truehd.sh's shared build.
# The configure line, prefix included, is embedded in the binary: keep /tmp/ffdca.
RUN git clone -q --depth 1 -b n4.4.4 https://git.ffmpeg.org/ffmpeg.git /tmp/ffmpeg \
 && cd /tmp/ffmpeg && ./configure --cross-prefix=arm-linux-gnueabi- \
      --enable-cross-compile --arch=arm --target-os=linux --prefix=/tmp/ffdca \
      --disable-everything --enable-decoder=dca --enable-muxer=spdif \
      --enable-avformat --disable-avfilter --disable-swresample \
      --disable-swscale --disable-avdevice --disable-postproc \
      --disable-network --disable-programs --disable-doc --disable-autodetect \
      --disable-pthreads --disable-asm \
      --enable-static --disable-shared --enable-pic >/dev/null \
 && { grep -q "CONFIG_DCA_DECODER 1" config.h \
      || { echo "ERROR: FFmpeg configured without the dca decoder"; exit 1; }; } \
 && { grep -q "CONFIG_SPDIF_MUXER 1" config.h \
      || { echo "ERROR: FFmpeg configured without the spdif muxer"; exit 1; }; } \
 && make -j"$(nproc)" install >/dev/null \
 && rm -rf /tmp/ffmpeg
DOCKERFILE_EOF

docker run --rm -i \
  -v "$HERE/src":/work:ro \
  -v "$OUT":/out \
  dts25-dtsdec-build /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
    cd /work

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
    arm-linux-gnueabi-gcc -shared -fPIC -O2 -Wall -o /out/libgstdtsdec.so gstdtsdec.c lgroute.c lgroute-control.c \
      -I/tmp/ffdca/include \
      -DVERSION='"1.22.0-webosdts"' \
      -DPACKAGE='"gst-plugins-bad"' \
      -DGST_PACKAGE_NAME='"WebOS DTS restore"' \
      -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore"' \
      $CF /tmp/ffdca/lib/libavformat.a /tmp/ffdca/lib/libavcodec.a \
      /tmp/ffdca/lib/libavutil.a $LB -lm \
      -Wl,--exclude-libs,ALL -Wl,--no-undefined

    echo "=== BUILT (must be ARM, 0x5000200 soft-float) ==="
    arm-linux-gnueabi-readelf -h /out/libgstdtsdec.so | grep -E "Machine|Flags"
    arm-linux-gnueabi-readelf -d /out/libgstdtsdec.so | grep -E "NEEDED|RUNPATH|RPATH"
    echo "=== max GLIBC (must be <= 2.35) ==="
    arm-linux-gnueabi-objdump -T /out/libgstdtsdec.so | grep -oE "GLIBC_[0-9.]+" | sort -V | tail -1
    echo "=== exported FFmpeg symbols (must be 0) ==="
    n=$(arm-linux-gnueabi-objdump -T /out/libgstdtsdec.so | grep -cE " (av|ff|avpriv|avcodec|swr)_" || true)
    echo "$n"; [ "$n" = 0 ] || { echo "ERROR: FFmpeg symbols leak from the plugin"; exit 1; }

    arm-linux-gnueabi-gcc -std=c11 -O2 -Wall -Wextra -Werror -DLGROUTE_CONTROL_MAIN \
      -o /out/lgroute-control lgroute-control.c
    arm-linux-gnueabi-readelf -h /out/lgroute-control | grep -E "Machine|Flags"

CONTAINER_EOF

ls -la "$OUT"
