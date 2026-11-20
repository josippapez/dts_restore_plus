#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
qemu=${QEMU_ARM:-qemu-arm}
image=${DTSDEC_BUILD_IMAGE:-dts25-dtsdec-build}
command -v "$qemu" >/dev/null || { echo "Missing QEMU runner: $qemu" >&2; exit 1; }
docker image inspect "$image" >/dev/null || {
  echo "Missing $image: run webos25/restore/build.sh first" >&2
  exit 1
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/sysroot"

# Compile with the same armel GStreamer headers and static FFmpeg as build.sh.
# Export the matching shared libraries for QEMU's guest loader. Nothing is
# installed on the host and the source directory stays read-only in Docker.
docker run --rm -i --network none -v "$src:/source:ro" "$image" \
  /bin/bash -euo pipefail -s <<'CONTAINER' | tar -xf - -C "$work/sysroot"
export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig
cf=$(pkg-config --cflags gstreamer-audio-1.0 gstreamer-check-1.0)
lb=$(pkg-config --libs gstreamer-audio-1.0 gstreamer-check-1.0)
arm-linux-gnueabi-gcc -O2 -Wall -I/tmp/ffdca/include \
  -DVERSION='"test"' -DPACKAGE='"gst-plugins-bad"' \
  -DGST_PACKAGE_NAME='"test"' -DGST_PACKAGE_ORIGIN='"local"' \
  -o /test-mat-truehd /source/test/mat-truehd/test-mat-truehd.c \
  $cf /tmp/ffdca/lib/libavformat.a /tmp/ffdca/lib/libavcodec.a \
  /tmp/ffdca/lib/libavutil.a $lb -lm
arm-linux-gnueabi-readelf -h /test-mat-truehd | grep -E 'Machine:|Flags:' >&2
cd /
find lib/arm-linux-gnueabi -maxdepth 1 -name '*.so.*' -print0 \
  | tar -chf - --null -T - lib/ld-linux.so.3 test-mat-truehd
CONTAINER

ulimit -c 0
GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_1_0="$work/registry.bin" \
  G_DEBUG=fatal-warnings timeout 30s "$qemu" -L "$work/sysroot" "$work/sysroot/test-mat-truehd" \
  "$src/test/hdmv-ac3/fixtures/expected.truehd"
