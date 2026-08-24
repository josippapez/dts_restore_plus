#!/bin/bash
# Reproducible cross-build of LG's PUBLIC GStreamer 1.14.4 MPEG-TS demuxer
# (libgstmpegtsdemux.so) for the webOS-22-era "C2/G2/CS" profile.
#
# WHY 1.14.4 AND NOT THE 1.24 TREE: gst_plugin_check_version() rejects a plugin
# only when its recorded minor is GREATER than the host's
# (subprojects/gstreamer/gst/gstplugin.c:487), so a 1.14 plugin loads on a
# 1.18.5 TV while the committed 1.24 demux-out/ build does not.
#
# NO SOURCE PATCH. Unlike build-demux.sh (which must flip `dts_support` and
# un-#if-0 the TrueHD case in the 1.24 tree), LG's 1.14.4 tsdemux.c has DTS
# unconditionally: zero `#ifdef DTS_SUPPORT`, the sink template advertises
# "audio/x-dts; audio/x-dtsh; audio/x-dtse; audio/x-dtsl", and
# `case ST_PS_AUDIO_DTS:` sets audio/x-dts outright. The build ASSERTS those
# three facts and fails if a future source revision breaks them, rather than
# patching anything.
#
# Produces libgstmpegtsdemux.so for LG C2/G2/CS:
#   32-bit ARM EABI5 soft-float (arm-linux-gnueabi), e_flags 0x05000200,
#   ld-linux.so.3, built on debian:11-slim. GStreamer 1.14.4.
#
# BUILD SYSTEM: meson, as shipped by debian bullseye (0.56.2). NOT the
# meson==1.4.2 pip pin used by build-demux.sh -- the 1.14.4 trees declare
# `meson_version : '>= 0.40.1'` and use pre-feature-option syntax
# (`disable_introspection`, `use_orc`) that modern meson no longer configures.
# bullseye's 0.56.2 is era-appropriate and needs no pip at all.
#
# THIS ARTIFACT IS NOT DEVICE-VERIFIED and is deliberately wired into NOTHING:
# no payload, no installer, no release path. See .claude/rules/releasing.md --
# a committed .so may only ship once it has been verified on a real TV.
#
# Usage: ./build-ts114.sh [src-cache-dir] [out-dir]
# Requires: docker or podman, rsync, git.
# Env: CLEAN=0 keeps the container image (faster re-runs; default removes it).
set -euo pipefail

SRCDIR=${1:-${TMPDIR:-/tmp}/ts114-src}
OUT=${2:-$(pwd)/ts114-out}
SNAPSHOT=20250601T000000Z   # last debian snapshot with armel in bullseye/main
BRANCH=lg
IMAGE=ts114-armel
CLEAN=${CLEAN:-1}

# `docker` is a shell alias to podman on the maintainer's machine, which a
# #!/bin/bash script never sees. Resolve a real binary instead of assuming.
if command -v docker >/dev/null 2>&1; then
  OCI=docker
elif command -v podman >/dev/null 2>&1; then
  OCI=podman
else
  echo "need docker or podman on PATH" >&2; exit 1
fi

# Baseline for the ABI comparison: a committed CX binary that already runs on
# the target-era TV. Optional -- skipped if this script is run outside the repo.
BASELINE=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)/gst/libgstmatroska.so

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
mkdir -p "$OUT" "$CTX/src" "$SRCDIR"

# ---------------------------------------------------------------------------
# Source: LG's public 1.14.4 trees. Shallow, pinned to the `lg` branch, cached
# between runs so a re-run does not re-download ~100 MB.
# ---------------------------------------------------------------------------
for p in gstreamer gst-plugins-base gst-plugins-bad; do
  if [ ! -d "$SRCDIR/$p/.git" ]; then
    git clone --depth 1 -b "$BRANCH" "https://github.com/lgstreamer/$p.git" "$SRCDIR/$p"
  fi
  ver=$(sed -n 's/^AC_INIT(\[[^]]*\],\[\([0-9.]*\)\].*/\1/p' "$SRCDIR/$p/configure.ac" | head -1)
  [ "$ver" = "1.14.4" ] || { echo "$p is $ver, expected 1.14.4"; exit 1; }
  echo "$p: $ver @ $(git -C "$SRCDIR/$p" rev-parse HEAD)"
  rsync -a --exclude .git "$SRCDIR/$p" "$CTX/src/"
done

# ---------------------------------------------------------------------------
# NO-PATCH ASSERTION (the inverse of build-demux.sh's patch verification):
# prove DTS is already unconditional, so nothing needs editing.
# ---------------------------------------------------------------------------
TSDEMUX="$CTX/src/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"
echo "=== no-patch verification ==="
if grep -n 'DTS_SUPPORT' "$TSDEMUX"; then
  echo "UNEXPECTED: 1.14.4 tsdemux.c gates DTS behind DTS_SUPPORT -- stop and re-plan"; exit 1
fi
grep -n 'audio/x-dts; audio/x-dtsh' "$TSDEMUX" \
  || { echo "UNEXPECTED: DTS missing from the sink caps template"; exit 1; }
grep -n 'case ST_PS_AUDIO_DTS:' "$TSDEMUX" \
  || { echo "UNEXPECTED: ST_PS_AUDIO_DTS case absent"; exit 1; }
echo "=== no-patch OK (DTS unconditional; source used verbatim) ==="

# ---------------------------------------------------------------------------
# LG meson bug (build-system only -- NOT a source patch): gst-libs/gst/basedrm's
# meson.build closes its "if xml2_dep.found() and libsoup_dep.found()" guard
# before the declare_dependency() that references the library, so configuring
# without libxml2/libsoup dies with 'Unknown variable "gstbasedrm"'. Same defect
# and same one-line fix build-demux.sh applies to gst-libs/gst/mpdclient in the
# 1.24 tree: move the lone endif to end of file.
# ---------------------------------------------------------------------------
python3 - "$CTX/src/gst-plugins-bad/gst-libs/gst/basedrm/meson.build" <<'BASEDRM_PY'
import sys
p = sys.argv[1]
s = open(p).read()
if s.count('\nendif\n') == 1 and not s.rstrip('\n').endswith('endif'):
    s = s.replace('\nendif\n', '\n', 1).rstrip('\n') + '\nendif\n'
    open(p, 'w').write(s)
BASEDRM_PY

cat > "$CTX/cross-armel.txt" <<'EOF'
[binaries]
c = 'arm-linux-gnueabi-gcc'
cpp = 'arm-linux-gnueabi-g++'
ar = 'arm-linux-gnueabi-ar'
strip = 'arm-linux-gnueabi-strip'
objcopy = 'arm-linux-gnueabi-objcopy'
ld = 'arm-linux-gnueabi-ld'
pkgconfig = 'pkg-config'

[properties]
pkg_config_libdir = '/opt/gst/lib/pkgconfig:/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig'

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
meson --version

# Re-confirm inside the container that the source is unpatched stock 1.14.4.
echo "=== in-container no-patch check ==="
! grep -q 'DTS_SUPPORT' "$SRC/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"
grep -c 'audio/x-dts' "$SRC/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"

# 1.14.4 predates meson feature options: the knobs are `disable_*` booleans and
# a `use_orc` combo, not -Dauto_features / -Dplugin=enabled.
COMMON="--cross-file $CROSS --prefix $PREFIX --libdir lib --buildtype release
  -Ddisable_introspection=true -Ddisable_examples=true -Ddisable_gtkdoc=true"

meson setup "$WORK/core" "$SRC/gstreamer" $COMMON \
  -Dbuild_tools=false -Ddisable_libunwind=true \
  -Dwith-ptp-helper-permissions=setuid-root
# ^ NOT 'none': 1.14.4's helpers/meson.build accepts 'none' but its
# ptp_helper_post_install.sh has no 'none' case and exits 2, failing the
# install. 'setuid-root' takes a branch the script handles (its chown/chmod are
# `|| true`), and gst-ptp-helper is irrelevant to this plugin either way.
ninja -C "$WORK/core" install

# NOT -Duse_orc=no: that branch of 1.14.4's meson.build never defines
# `orc_dep`, yet gst/audiomixer/meson.build lists it unconditionally
# ("ERROR: Unknown variable orc_dep"). Leaving orc on 'auto' with orc absent
# from the image takes the supported not-found path: have_orcc is false and the
# pre-generated *-dist.c backup C code is compiled instead.
meson setup "$WORK/base" "$SRC/gst-plugins-base" $COMMON
ninja -C "$WORK/base" install

# Configure all of -bad (1.14 has no per-plugin switches; plugins without deps
# simply configure out), then build ONLY the mpegtsdemux target.
meson setup "$WORK/bad" "$SRC/gst-plugins-bad" $COMMON
ninja -C "$WORK/bad" gst/mpegtsdemux/libgstmpegtsdemux.so

cp "$WORK/bad/gst/mpegtsdemux/libgstmpegtsdemux.so" "$OUT/"
arm-linux-gnueabi-strip --strip-unneeded "$OUT/libgstmpegtsdemux.so"

SO="$OUT/libgstmpegtsdemux.so"
echo "=== ARTIFACT VERIFICATION ==="
echo "--- file"; file "$SO"
echo -n "--- e_flags: "; od -An -tx4 -j36 -N4 "$SO"
echo -n "--- max GLIBC: "; arm-linux-gnueabi-objdump -T "$SO" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
echo "--- NEEDED:"; arm-linux-gnueabi-readelf -d "$SO" | grep NEEDED
echo "--- interpreter/soname:"; arm-linux-gnueabi-readelf -d "$SO" | grep SONAME || true
echo -n "--- audio/x-dts strings: "; strings "$SO" | grep -c 'audio/x-dts'
echo -n "--- DTS audio strings: "; strings "$SO" | grep -c 'DTS audio'
echo -n "--- recorded GStreamer version: "; strings "$SO" | grep -E '^1\.14\.[0-9]+$' | sort -u | tr '\n' ' '; echo

if [ -f /baseline/libgstmatroska.so ]; then
  echo "=== BASELINE (gst/libgstmatroska.so) COMPARISON ==="
  echo "--- file"; file /baseline/libgstmatroska.so
  echo -n "--- baseline max GLIBC: "
  arm-linux-gnueabi-objdump -T /baseline/libgstmatroska.so | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
  echo "--- baseline NEEDED:"; arm-linux-gnueabi-readelf -d /baseline/libgstmatroska.so | grep NEEDED
fi
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
      meson ninja-build pkg-config flex bison python3 \\
      libglib2.0-dev-bin libglib2.0-dev:armel zlib1g-dev:armel \\
      file binutils && \\
    rm -rf /var/lib/apt/lists/*
COPY cross-armel.txt /cross-armel.txt
COPY build-inside.sh /build-inside.sh
RUN chmod +x /build-inside.sh
EOF

$OCI build --build-arg SNAPSHOT=$SNAPSHOT -t "$IMAGE" "$CTX"

BASEMOUNT=()
[ -f "$BASELINE" ] && BASEMOUNT=(-v "$(dirname "$BASELINE")":/baseline:ro)
$OCI run --rm -v "$CTX/src":/src:ro -v "$OUT":/out "${BASEMOUNT[@]}" "$IMAGE" /build-inside.sh

# Ship the recipe next to the artifact, as truehd-out/build-truehd.sh does.
cp "$0" "$OUT/build-ts114.sh"

if [ "$CLEAN" = "1" ]; then
  $OCI rmi -f "$IMAGE" >/dev/null 2>&1 || true
  echo "removed image $IMAGE (CLEAN=0 to keep it for faster re-runs)"
fi
echo "Artifacts in $OUT"
