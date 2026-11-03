#!/usr/bin/env bash
#
# build.sh CAPTURE_DIR... -- build out/earc_route-5.4.ko and out/earc_route-6.12.ko
# for the webOS 25 5.4.268 and 6.12.44 kernel generations (each one only if a
# capture of it is given) and check them against private kernel captures.
#
# Each CAPTURE_DIR holds kernel.bin, kallsyms.txt and stock-module.ko from one
# TV (private LG files: keep them outside this repository); the generation of
# each comes from its stock module. kallsyms.txt must be the live /proc/kallsyms
# addresses (the 6.12 survey captures were 0x10000 too high and are corrected).
# They are mounted read-only; the modules do not embed anything from them.
#
# Runs build.py in a linux/arm64 container: debian:12-slim (GCC 12.2) with
# gcc, binutils and python3 installed on the fly, or EARC_BUILD_IMAGE if that
# image already has aarch64-linux-gnu-gcc, readelf, nm and python3.
#
# Produces in webos25/kmod/out/: earc_route-<generation>.ko, earc_route-<generation>.json
# (sha256, ABI, per-kernel export proof) and build/ intermediates.
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
