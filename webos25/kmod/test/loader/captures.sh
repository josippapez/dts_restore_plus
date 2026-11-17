#!/bin/sh
# captures.sh [CAPTURE...] -- the shipped loader on each private capture: its own stock
# module and kallsyms, the committed .ko files, insmod stubbed. A kernel the boot script
# accepts must get its generation's build and every required symbol.
# Default: every capture in test/captures/. With none, prints SKIP and passes.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
if [ $# -eq 0 ]; then
  for dir in "$HERE"/../captures/*/; do [ -f "$dir/kernel.bin" ] && set -- "$@" "$(cd "$dir" && pwd)"; done
fi
[ $# -gt 0 ] || { echo "SKIP: no private kernel captures"; exit 0; }

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
KO=${EARC_KO_DIR:-$HERE/../../../restore/kmod-out}
mkdir -p "$BUILD/app" "$BUILD/bin"
cp "$HERE/../../earc-route-load.sh" "$KO"/earc_route-*.ko "$BUILD/app/"
printf '#!/bin/sh\necho 0\n' > "$BUILD/bin/id"
printf '#!/bin/sh\nexit 0\n' > "$BUILD/bin/insmod"
printf '#!/bin/sh\n:\n' > "$BUILD/bin/dmesg"
chmod +x "$BUILD/bin/"*
for dir in "$@"; do
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
