#!/bin/bash
# Reproducible cross-build of LG webOS-25 GStreamer 1.24 demuxers with DTS
# re-enabled BOTH at compile time (-Ddca=true => #ifdef DTS_SUPPORT) AND at
# RUNTIME via a 2-line source patch that flips the default of the GObject
# property `dts_support` from FALSE to TRUE (LG never sets it true on-device,
# so mp4 DTS fell back to audio/x-gst-fourcc-dtsc).
#
# Produces libgstisomp4.so + libgstmpegtsdemux.so for LG C5 (webOS 25):
#   32-bit ARM EABI5 soft-float (arm-linux-gnueabi), e_flags 0x05000200,
#   ld-linux.so.3, glibc <= 2.35 (built on debian:11-slim). GStreamer 1.24.
#
# Usage: ./build-demux.sh <path-to-webos25-monorepo> <out-dir>
# Requires: docker (or podman aliased to docker), rsync.
set -euo pipefail

MONOREPO=${1:-/Users/josippapez/dts_restore_work/scratch/gstreamer-webos-25}
OUT=${2:-$(pwd)/demux-out}
CTX=$(mktemp -d)
SNAPSHOT=20250601T000000Z   # last debian snapshot with armel in bullseye/main

mkdir -p "$OUT" "$CTX/src"
for p in gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad; do
  rsync -a "$MONOREPO/subprojects/$p" "$CTX/src/"
done

# ---------------------------------------------------------------------------
# 2-LINE DTS RUNTIME PATCH: flip the default of the `dts_support` property
# from FALSE to TRUE in both demuxers (only the default-init assignments,
# inside #ifdef DTS_SUPPORT). Applied to the copied source, then verified.
# ---------------------------------------------------------------------------
QTDEMUX="$CTX/src/gst-plugins-good/gst/isomp4/qtdemux.c"
TSDEMUX="$CTX/src/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"

perl -0pi -e 's/qtdemux->dts_support = FALSE;/qtdemux->dts_support = TRUE;/g' "$QTDEMUX"
perl -0pi -e 's/demux->dts_support = FALSE;/demux->dts_support = TRUE;/g'     "$TSDEMUX"

echo "=== DTS patch verification ==="
for f in "$QTDEMUX" "$TSDEMUX"; do
  echo "--- $f"
  grep -n 'dts_support = TRUE'  "$f" || { echo "PATCH FAILED: no TRUE in $f"; exit 1; }
  if grep -n 'dts_support = FALSE' "$f"; then
    echo "PATCH FAILED: dts_support = FALSE still present in $f"; exit 1
  fi
done
echo "=== DTS patch OK (both files: dts_support = TRUE, no remaining FALSE) ==="

# Minimal patch for an LG meson bug: gst-libs/gst/mpdclient/meson.build uses
# gstmpdclient/pkg_name outside the "if xml2_dep.found()" guard, which breaks
# configuration when dash is disabled. Move the endif to end of file.
python3 - "$CTX/src/gst-plugins-bad/gst-libs/gst/mpdclient/meson.build" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if s.count('\nendif\n') == 1 and not s.rstrip('\n').endswith('endif'):
    s = s.replace('\nendif\n', '\n', 1).rstrip('\n') + '\nendif\n'
    open(p, 'w').write(s)
PY

cat > "$CTX/cross-armel.txt" <<'EOF'
[binaries]
c = 'arm-linux-gnueabi-gcc'
cpp = 'arm-linux-gnueabi-g++'
ar = 'arm-linux-gnueabi-ar'
strip = 'arm-linux-gnueabi-strip'
objcopy = 'arm-linux-gnueabi-objcopy'
ld = 'arm-linux-gnueabi-ld'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['/opt/gst/lib/pkgconfig', '/usr/lib/arm-linux-gnueabi/pkgconfig', '/usr/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

cat > "$CTX/build-inside.sh" <<'EOF'
#!/bin/bash
set -euo pipefail
SRC=/src; WORK=/work; PREFIX=/opt/gst; CROSS=/cross-armel.txt; OUT=/out
mkdir -p "$WORK" "$OUT"
export PATH="$PREFIX/bin:$PATH"

# Re-confirm the DTS patch is present in the source seen inside the container.
echo "=== in-container DTS patch check ==="
grep -n 'qtdemux->dts_support = TRUE' "$SRC/gst-plugins-good/gst/isomp4/qtdemux.c"
grep -n 'demux->dts_support = TRUE'   "$SRC/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"

COMMON="--cross-file $CROSS --prefix $PREFIX --libdir lib --buildtype release
  -Dexamples=disabled -Dtests=disabled -Ddoc=disabled
  -Dnls=disabled -Dglib-asserts=disabled -Dglib-checks=disabled
  -Dgobject-cast-checks=disabled"

meson setup "$WORK/core" "$SRC/gstreamer" $COMMON \
  -Dintrospection=disabled \
  -Dtools=disabled -Dbenchmarks=disabled -Dbash-completion=disabled \
  -Dcoretracers=disabled -Dcheck=disabled -Dlibunwind=disabled -Dlibdw=disabled \
  -Ddbghelp=disabled -Dptp-helper=disabled -Dextra-checks=disabled
ninja -C "$WORK/core" install

meson setup "$WORK/base" "$SRC/gst-plugins-base" $COMMON \
  -Dintrospection=disabled \
  -Dauto_features=disabled -Dtools=disabled -Dorc=disabled
ninja -C "$WORK/base" install

meson setup "$WORK/good" "$SRC/gst-plugins-good" $COMMON \
  -Dauto_features=disabled -Disomp4=enabled -Ddca=true -Dorc=disabled
ninja -C "$WORK/good" install

meson setup "$WORK/bad" "$SRC/gst-plugins-bad" $COMMON \
  -Dintrospection=disabled \
  -Dauto_features=disabled -Dmpegtsdemux=enabled -Ddca=true -Dorc=disabled
ninja -C "$WORK/bad" install

cp "$PREFIX/lib/gstreamer-1.0/libgstisomp4.so" "$OUT/"
cp "$PREFIX/lib/gstreamer-1.0/libgstmpegtsdemux.so" "$OUT/"
arm-linux-gnueabi-strip --strip-unneeded "$OUT/libgstisomp4.so" "$OUT/libgstmpegtsdemux.so"

for so in "$OUT"/libgstisomp4.so "$OUT"/libgstmpegtsdemux.so; do
  echo "--- $so"
  file "$so"
  echo -n "e_flags: "; od -An -tx4 -j36 -N4 "$so"
  echo -n "max GLIBC: "; arm-linux-gnueabi-objdump -T "$so" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
  echo "NEEDED:"; arm-linux-gnueabi-readelf -d "$so" | grep NEEDED
  echo -n "x-dts strings: "; strings "$so" | grep -c 'audio/x-dts'
  echo -n "DTS audio strings: "; strings "$so" | grep -c 'DTS audio'
done
echo "BUILD OK"
EOF
chmod +x "$CTX/build-inside.sh"

cat > "$CTX/Dockerfile" <<EOF
FROM debian:11-slim
ARG SNAPSHOT=$SNAPSHOT
RUN dpkg --add-architecture armel && \\
    printf 'deb http://snapshot.debian.org/archive/debian/%s bullseye main\\n' "\$SNAPSHOT" > /etc/apt/sources.list && \\
    rm -f /etc/apt/sources.list.d/*.list && \\
    printf 'Package: *\\nPin: origin "snapshot.debian.org"\\nPin-Priority: 1001\\n' > /etc/apt/preferences.d/snapshot && \\
    apt-get -o Acquire::Check-Valid-Until=false update && \\
    DEBIAN_FRONTEND=noninteractive apt-get -y --allow-downgrades dist-upgrade && \\
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \\
      build-essential gcc-arm-linux-gnueabi g++-arm-linux-gnueabi \\
      ninja-build pkg-config flex bison \\
      python3 python3-pip python3-setuptools python3-wheel \\
      libglib2.0-dev-bin libglib2.0-dev:armel zlib1g-dev:armel \\
      file binutils && \\
    rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir 'meson==1.4.2'
COPY cross-armel.txt /cross-armel.txt
COPY build-inside.sh /build-inside.sh
RUN chmod +x /build-inside.sh
EOF

docker build --build-arg SNAPSHOT=$SNAPSHOT -t demux-armel "$CTX"
docker run --rm -v "$CTX/src":/src:ro -v "$OUT":/out demux-armel /build-inside.sh
echo "Artifacts in $OUT"
