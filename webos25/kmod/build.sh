#!/usr/bin/env bash
#
# build.sh CAPTURE_DIR... -- build out/earc_route.ko for the webOS 25 5.4.268
# kernel generation and check it against private kernel captures.
#
# Each CAPTURE_DIR holds kernel.bin, kallsyms.txt and stock-module.ko from one
# TV (private LG files: keep them outside this repository). They are mounted
# read-only; the module does not embed anything from them.
#
# Runs build.py in a linux/arm64 container: debian:12-slim (GCC 12.2) with
# gcc, binutils and python3 installed on the fly, or EARC_BUILD_IMAGE if that
# image already has aarch64-linux-gnu-gcc, readelf, nm and python3.
#
# Produces in webos25/kmod/out/: earc_route.ko, earc_route.json (sha256, ABI,
# per-kernel export proof) and build/ intermediates.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${EARC_BUILD_IMAGE:-debian:12-slim}"
if [ $# -lt 1 ]; then
  echo "usage: $0 CAPTURE_DIR..." >&2
  exit 2
fi

mounts=()
inputs=()
index=0
for capture in "$@"; do
  capture="$(cd "$capture" && pwd)"
  for file in kernel.bin kallsyms.txt stock-module.ko; do
    [ -f "$capture/$file" ] || { echo "missing $capture/$file" >&2; exit 2; }
  done
  mounts+=(-v "$capture:/captures/$index:ro")
  inputs+=("/captures/$index")
  index=$((index + 1))
done

mkdir -p "$HERE/out"
docker run --rm -i --platform linux/arm64 \
  -v "$HERE:/kmod" "${mounts[@]}" \
  "$IMAGE" /bin/sh -euc '
    if ! command -v aarch64-linux-gnu-gcc >/dev/null || ! command -v python3 >/dev/null; then
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null
      apt-get install -y -qq --no-install-recommends gcc binutils python3 >/dev/null
    fi
    aarch64-linux-gnu-gcc --version | head -n 1
    python3 /kmod/build.py --output /kmod/out "$@"
  ' sh "${inputs[@]}"
