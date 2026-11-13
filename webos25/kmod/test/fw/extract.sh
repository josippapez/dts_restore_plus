#!/bin/sh
# extract.sh NAME RELEASE -- inside the earc-fw image: /out/.zip/NAME.zip ->
# /out/NAME/{kernel.bin,kallsyms.txt,stock-module.ko}. 5.4 firmwares keep the
# rootfs in rootfs.pak; 6.12 ones in an Android super image (super.pak).
set -eu
name=$1 release=$2 work=/tmp/$1 out=/out/$1
mkdir -p "$work" && cd "$work"
unzip -q -o "/out/.zip/$name.zip" '*.epk' -d zip
epk=$(find zip -name '*.epk' | head -n 1)
(cd /opt/epk2extract/build_linux && ./epk2extract "$work/$epk" >"$work/epk.log" 2>&1) || true
rootfs=$(find zip -name rootfs.pak -size +100M | head -n 1)
if [ -z "$rootfs" ]; then
  super=$(find zip -name super.pak | head -n 1)
  [ -n "$super" ] || { echo "$name: neither rootfs.pak nor super.pak"; exit 1; }
  python3 /fw/lpunpack.py "$super" parts >/dev/null
  rootfs=parts/rootfs.img
fi
unsquashfs -q -f -d rfs "$rootfs" 'boot/*' "lib/modules/$release/kernel/aspectratiodrv.ko" \
  "usr/lib/modules/$release/kernel/aspectratiodrv.ko" >/dev/null 2>&1 || true
img=rfs/boot/Image.lz4-$release
ko=$(find rfs -name aspectratiodrv.ko -path "*/$release/*" | head -n 1)
[ -f "$img" ] && [ -n "$ko" ] || { echo "$name: no kernel $release or stock module"; find rfs/boot; exit 1; }
mkdir -p "$out.tmp"
python3 /fw/unlz4p.py "$img" "$out.tmp/kernel.bin" >/dev/null
cp "$ko" "$out.tmp/stock-module.ko"
# 6.12 stores relative kallsyms: give the link base so _stext reads as on the TV.
case $release in 6.*) base='--base-address 0xffffffc080000000' ;; *) base= ;; esac
# shellcheck disable=SC2086
kallsyms-finder $base "$out.tmp/kernel.bin" >"$out.tmp/kallsyms.txt" 2>/dev/null
grep -q ' T _stext$' "$out.tmp/kallsyms.txt" || { echo "$name: kallsyms has no _stext"; exit 1; }
rm -rf "$out" && mv "$out.tmp" "$out"
echo "$name: $release, $(wc -l < "$out/kallsyms.txt") symbols"
rm -rf "$work"
