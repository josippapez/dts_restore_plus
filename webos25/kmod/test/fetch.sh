#!/bin/sh
# fetch.sh [NAME...] -- download the firmwares in kernels.tsv and extract each
# one's kernel into test/kernels/NAME/{kernel.bin,kallsyms.txt,stock-module.ko}.
#
# test/kernels/ is git-ignored: LG's files never enter the repository. Needs
# curl and docker; the extraction image (fw/Dockerfile) builds once. A zip is
# fetched as FETCH_CONNECTIONS (20) parallel ranges, checked against its
# sha256 and deleted after extraction, so only one is ever on disk.
# Already extracted kernels are skipped.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/kernels
mkdir -p "$OUT/.zip"
docker image inspect earc-fw >/dev/null 2>&1 || docker build -q -t earc-fw "$HERE/fw" >/dev/null

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

grep -v '^#' "$HERE/kernels.tsv" | while IFS="$(printf '\t')" read -r name release _ _ zipsha path; do
  if [ $# -gt 0 ]; then case " $* " in *" $name "*) ;; *) continue ;; esac; fi
  if [ -f "$OUT/$name/kallsyms.txt" ]; then echo "$name: present"; continue; fi
  zip=$OUT/.zip/$name.zip
  url=https://tv.slada.sk/$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$path")
  size=$(curl -fsSI "$url" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: //p' | tail -n1)
  n=${FETCH_CONNECTIONS:-20} part=$((size / n + 1)) i=0
  : > "$zip.cfg"
  while [ "$i" -lt "$n" ]; do
    printf 'url = "%s"\nrange = "%d-%d"\noutput = "%s.%02d"\n' "$url" $((i * part)) $((i * part + part - 1)) "$zip" "$i" >> "$zip.cfg"
    i=$((i + 1))
  done
  # HTTP/1.1: over HTTP/2 curl multiplexes every range onto one connection.
  curl -fsS --http1.1 --retry 5 --parallel --parallel-max "$n" -K "$zip.cfg" || true
  cat "$zip".[0-9][0-9] > "$zip"
  rm -f "$zip".[0-9][0-9] "$zip.cfg"
  if [ "$(sha256 "$zip")" != "$zipsha" ]; then echo "$name: download incomplete (sha256 mismatch)"; rm -f "$zip"; continue; fi
  docker run --rm -v "$OUT:/out" earc-fw sh /fw/extract.sh "$name" "$release" || true
  rm -f "$zip"
done
