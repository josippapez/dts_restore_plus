#!/bin/sh
# run.sh -- host test of earc_decode.h against private kernel captures.
#
#   EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="dir dir"] [EARC_612_DIRS="dir dir"] sh test/run.sh
#
# Every capture fetch.sh put in test/kernels/ (from kernels.tsv) is included too.
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
for dir in "$HERE"/kernels/*/; do [ -d "$dir" ] && add "${dir%/}"; done
if [ -z "$ARGS" ]; then
  echo "SKIP: no private kernel captures (set EARC_C5_DIR, EARC_G5_DIR, EARC_EXTRA_DIRS, EARC_612_DIRS)"
  exit 0
fi

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
  -o "$BUILD/test_decode" "$HERE/test_decode.c"
# shellcheck disable=SC2086 # ARGS is a list of options and directories
"$BUILD/test_decode" $ARGS || status=1

# The shipped loader on each capture: its own stock module and kallsyms, the
# committed .ko files, insmod stubbed. A kernel the boot script accepts must
# get its generation's build and every required symbol.
KO=${EARC_KO_DIR:-$HERE/../../restore/kmod-out}
mkdir -p "$BUILD/app" "$BUILD/bin"
cp "$HERE/../earc-route-load.sh" "$KO"/earc_route-*.ko "$BUILD/app/"
printf '#!/bin/sh\necho 0\n' > "$BUILD/bin/id"
printf '#!/bin/sh\nexit 0\n' > "$BUILD/bin/insmod"
printf '#!/bin/sh\n:\n' > "$BUILD/bin/dmesg"
chmod +x "$BUILD/bin/"*
set -- $ARGS
while [ $# -ge 3 ]; do
  dir=$3; shift 3
  [ -f "$dir/stock-module.ko" ] || { echo "SKIP loader $dir: no stock-module.ko"; continue; }
  release=$(LC_ALL=C grep -a -o -m1 'Linux version [^ ]*' "$dir/kernel.bin" | cut -d' ' -f3)
  case $release in
    5.4.268-329.ptl4tv.[25]|5.4.268-320) want=earc_route-5.4 ;;
    # Realtek audio stack: none of the module's symbols, so the loader must refuse.
    6.12.*qilian*) want=refuse ;;
    6.12.*) want=earc_route-6.12 ;;
    *) want= ;;
  esac
  root=$BUILD/root; rm -rf "$root"; mkdir -p "$root/proc" "$root/lib/modules/$release"
  cp "$dir/stock-module.ko" "$root/lib/modules/$release/"
  cp "$dir/kallsyms.txt" "$root/proc/kallsyms"
  printf '#!/bin/sh\necho %s\n' "$release" > "$BUILD/bin/uname"; chmod +x "$BUILD/bin/uname"
  out=$(PATH="$BUILD/bin:$PATH" EARC_LOADER_ROOT="$root" sh "$BUILD/app/earc-route-load.sh" 2>&1) || true
  check=$(PATH="$BUILD/bin:$PATH" EARC_LOADER_ROOT="$root" sh "$BUILD/app/earc-route-load.sh" --check 2>&1 | tail -n 1) || true
  echo "GATE $dir ($release): $check"
  if [ "$want" = refuse ]; then
    if printf '%s\n' "$out" | grep -q 'REFUSED: kallsyms:' && ! printf '%s\n' "$out" | grep -q 'loading '; then
      echo "PASS loader $dir ($release): refused before insmod: $(printf '%s\n' "$out" | tail -n1)"
    else
      echo "FAIL loader $dir ($release): not refused: $(printf '%s\n' "$out" | tail -n1)"; status=1
    fi
  elif [ -z "$want" ]; then
    echo "INFO loader $dir ($release, not offered by the boot script): $(printf '%s\n' "$out" | tail -n1)"
  elif printf '%s\n' "$out" | grep -qE "loading $want[.-]"; then
    echo "PASS loader $dir ($release): picks $(printf '%s\n' "$out" | grep -oE 'earc_route-[0-9a-z.-]+\.ko' | head -n1), every symbol present"
  else
    echo "FAIL loader $dir ($release): $(printf '%s\n' "$out" | tail -n1)"; status=1
  fi
done
exit "${status:-0}"
