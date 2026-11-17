#!/usr/bin/env bash
#
# build-c1.sh — the webOS 6 (LG C1, GStreamer 1.16.2, glibc 2.28) build of dtsdec, for
# DTS passthrough with the 4.4 earc_route module (webos25/kmod/target/legacy-audio).
#
# Same source as build.sh, compiled against Debian buster's GStreamer 1.14 and glibc
# 2.28: gst_plugin_check_version() loads an older-minor plugin on a newer core, and a
# binary linked against glibc 2.28 runs on the C1's 2.28. -DDTSDEC_C1 selects the
# webOS 6 route (src/c1route.c) in place of webOS 25's (src/lgroute.c).
#
# Produces webos25/restore/c1-out/libgstdtsdec.so. EXPERIMENTAL: no C1 has run it;
# it ships behind the app's opt-in only (see RELEASING.md).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/c1-out}"
mkdir -p "$OUT"

docker build -t dts25-dtsdec-c1-build - <<'DOCKERFILE_EOF'
FROM debian:10-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN SNAPSHOT=20240101T000000Z \
 && rm -f /etc/apt/sources.list.d/* \
 && printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s buster main\n' \
      "$SNAPSHOT" > /etc/apt/sources.list \
 && printf 'Package: *\nPin: origin "snapshot.debian.org"\nPin-Priority: 1001\n' \
      > /etc/apt/preferences.d/snapshot \
 && dpkg --add-architecture armel \
 && apt-get -o Acquire::Check-Valid-Until=false update -qq \
 && apt-get -y -qq --allow-downgrades dist-upgrade \
 && apt-get install -y -qq --no-install-recommends \
      gcc-arm-linux-gnueabi pkg-config git ca-certificates make \
      libgstreamer1.0-dev:armel libgstreamer-plugins-base1.0-dev:armel libglib2.0-dev:armel \
 && rm -rf /var/lib/apt/lists/*
# Same static FFmpeg as build.sh: the dca decoder and the spdif muxer only.
RUN git clone -q --depth 1 -b n4.4.4 https://git.ffmpeg.org/ffmpeg.git /tmp/ffmpeg \
 && cd /tmp/ffmpeg && ./configure --cross-prefix=arm-linux-gnueabi- \
      --enable-cross-compile --arch=arm --target-os=linux --prefix=/tmp/ffdca \
      --disable-everything --enable-decoder=dca --enable-muxer=spdif \
      --enable-avformat --disable-avfilter --disable-swresample \
      --disable-swscale --disable-avdevice --disable-postproc \
      --disable-network --disable-programs --disable-doc --disable-autodetect \
      --disable-pthreads --disable-asm \
      --enable-static --disable-shared --enable-pic >/dev/null \
 && grep -q "CONFIG_DCA_DECODER 1" config.h && grep -q "CONFIG_SPDIF_MUXER 1" config.h \
 && make -j"$(nproc)" install >/dev/null \
 && rm -rf /tmp/ffmpeg
DOCKERFILE_EOF

docker run --rm -i -v "$HERE/src":/work:ro -v "$OUT":/out dts25-dtsdec-c1-build /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
    cd /work
    export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig
    CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)
    LB=$(pkg-config --libs   gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)
    # gst_audio_decoder_set_output_caps is in the C1's 1.16.2 libgstaudio but not buster's
    # 1.14: a stub of the same soname resolves it at link time only, keeping --no-undefined.
    arm-linux-gnueabi-gcc -std=gnu11 -shared -fPIC -O2 -Wall -Werror -D_GNU_SOURCE= -include c1compat.h \
      -o /out/libgstdtsdec.so gstdtsdec.c c1route.c lgroute-control.c \
      -I/tmp/ffdca/include \
      -DVERSION='"1.14.0-webosdts-c1"' -DPACKAGE='"gst-plugins-bad"' \
      -DGST_PACKAGE_NAME='"WebOS DTS restore"' \
      -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore"' \
      $CF /tmp/ffdca/lib/libavformat.a /tmp/ffdca/lib/libavcodec.a \
      /tmp/ffdca/lib/libavutil.a $LB -lm -ldl \
      -Wl,--exclude-libs,ALL -Wl,--no-undefined
    echo "=== BUILT (must be ARM, 0x5000200 soft-float) ==="
    arm-linux-gnueabi-readelf -h /out/libgstdtsdec.so | grep -E "Machine|Flags"
    arm-linux-gnueabi-readelf -d /out/libgstdtsdec.so | grep -E "NEEDED|RUNPATH|RPATH"
    echo "=== max GLIBC (must be <= 2.28) ==="
    arm-linux-gnueabi-objdump -T /out/libgstdtsdec.so | grep -oE "GLIBC_[0-9.]+" | sort -V | tail -1
    n=$(arm-linux-gnueabi-objdump -T /out/libgstdtsdec.so | grep -cE " (av|ff|avpriv|avcodec|swr)_" || true)
    [ "$n" = 0 ] || { echo "ERROR: FFmpeg symbols leak from the plugin"; exit 1; }
CONTAINER_EOF
ls -la "$OUT"
