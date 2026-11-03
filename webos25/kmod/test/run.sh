#!/bin/sh
# run.sh -- host test of earc_decode.h against private kernel captures.
#
#   EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="dir dir"] sh test/run.sh
#
# Each directory holds kernel.bin and kallsyms.txt from one TV (private LG
# files, never in this repository). EARC_C5_DIR must be the C5 33.31.68/69
# image and EARC_G5_DIR the G5 33.31.69 one; their sha256 is checked, so an
# oracle is never applied to the wrong kernel. Extra 5.4 kernels are decoded
# and printed without an oracle. With no captures the test skips, passing.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
C5_SHA256=49859c6561689f7fb62d683eed77c9e0d415b8a62fbf6b374626a2a1d5e44f94
G5_SHA256=c219532590e1d691a459ef8c0faab95745d04d36b6cbee4a32685409f3e7460e

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

oracle() {  # oracle OPTION DIR SHA256; directories must not contain spaces
  if [ -z "$2" ]; then return; fi
  if [ ! -f "$2/kernel.bin" ] || [ ! -f "$2/kallsyms.txt" ]; then
    echo "SKIP $1 $2: kernel.bin or kallsyms.txt missing"
  elif [ "$(sha256 "$2/kernel.bin")" != "$3" ]; then
    echo "SKIP $1 $2: kernel.bin is not the image this oracle describes"
  else
    ARGS="$ARGS $1 $2"
  fi
}
ARGS=
oracle --c5 "${EARC_C5_DIR:-}" "$C5_SHA256"
oracle --g5 "${EARC_G5_DIR:-}" "$G5_SHA256"
for dir in ${EARC_EXTRA_DIRS:-}; do ARGS="$ARGS --extra $dir"; done
if [ -z "$ARGS" ]; then
  echo "SKIP: no private kernel captures (set EARC_C5_DIR, EARC_G5_DIR, EARC_EXTRA_DIRS)"
  exit 0
fi

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
  -o "$BUILD/test_decode" "$HERE/test_decode.c"
# shellcheck disable=SC2086 # ARGS is a list of options and directories
"$BUILD/test_decode" $ARGS
