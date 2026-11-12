#!/bin/sh
# Build with the LG ARM soft-float SDK and run on a TV with the supported native ABI.
# SDK_ROOT=/path/to/arm-webos-linux-gnueabi_sdk-buildroot sh build-p7-rpu-tests.sh [output]
set -eu
SDK_ROOT=${SDK_ROOT:-$HOME/arm-webos-linux-gnueabi_sdk-buildroot}
SYSROOT=$SDK_ROOT/arm-webos-linux-gnueabi/sysroot
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$SDK_ROOT/bin/arm-linux-gcc" -O2 -Wall -Wextra -Wno-unused-parameter \
  $(pkg-config --cflags gstreamer-1.0) "$HERE/test-p7-rpu-normalize.c" \
  -o "${1:-./test-p7-rpu-normalize}" $(pkg-config --libs gstreamer-1.0) -ldl
