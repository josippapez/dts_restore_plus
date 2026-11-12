#!/bin/sh
# run.sh -- host test of earc_decode.h against private kernel captures.
#
#   EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="dir dir"] [EARC_612_DIRS="dir dir"] sh test/run.sh
#
# Each directory holds kernel.bin and kallsyms.txt from one TV (private LG
# files, never in this repository; for 6.12 kernels kallsyms.txt must be the
# corrected one, 0x10000 below the survey capture). The sha256 of each
# kernel.bin is computed here and passed to the test, which applies the oracle
# for exactly that image: a kernel in its table must decode to the hand-derived
# values (5.4: C5 33.31.68/69, G5 33.31.69; 6.12: C5 43.21.73/77, G5
# 43.21.71/77, G2 43.21.74), any other is decoded and printed without an
# oracle. EARC_C5_DIR, EARC_G5_DIR, EARC_EXTRA_DIRS and EARC_612_DIRS are
# equivalent apart from naming. With no captures the test skips, passing.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

ARGS=
add() {  # add DIR; directories must not contain spaces
  if [ -z "$1" ]; then return; fi
  if [ ! -f "$1/kernel.bin" ] || [ ! -f "$1/kallsyms.txt" ]; then
    echo "SKIP $1: kernel.bin or kallsyms.txt missing"
  else
    ARGS="$ARGS --kernel $(sha256 "$1/kernel.bin") $1"
  fi
}
add "${EARC_C5_DIR:-}"
add "${EARC_G5_DIR:-}"
for dir in ${EARC_EXTRA_DIRS:-} ${EARC_612_DIRS:-}; do add "$dir"; done
if [ -z "$ARGS" ]; then
  echo "SKIP: no private kernel captures (set EARC_C5_DIR, EARC_G5_DIR, EARC_EXTRA_DIRS, EARC_612_DIRS)"
  exit 0
fi

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
  -o "$BUILD/test_decode" "$HERE/test_decode.c"
# shellcheck disable=SC2086 # ARGS is a list of options and directories
"$BUILD/test_decode" $ARGS
