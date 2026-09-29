#!/bin/bash
# Reproducible cross-build of LG webOS-25 GStreamer 1.24 demuxers with DTS
# re-enabled BOTH at compile time (-Ddca=true => #ifdef DTS_SUPPORT) AND at
# RUNTIME via a 2-line source patch that flips the default of the GObject
# property `dts_support` from FALSE to TRUE (LG never sets it true on-device,
# so mp4 DTS fell back to audio/x-gst-fourcc-dtsc).
#
# Also patches matroska-demux so Dolby Vision profile 7 (UHD Blu-ray) MKVs
# are signalled as Dolby Vision instead of being downgraded to HDR10.
#
# Produces libgstisomp4.so + libgstmpegtsdemux.so + libgstmatroska.so for
# LG C5 (webOS 25):
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
# 3-LINE DTS RUNTIME PATCH: flip the default of the `dts_support` property
# from FALSE to TRUE in all three demuxers (only the default-init assignments,
# inside #ifdef DTS_SUPPORT). Applied to the copied source, then verified.
# ---------------------------------------------------------------------------
QTDEMUX="$CTX/src/gst-plugins-good/gst/isomp4/qtdemux.c"
TSDEMUX="$CTX/src/gst-plugins-bad/gst/mpegtsdemux/tsdemux.c"
MKVDEMUX="$CTX/src/gst-plugins-good/gst/matroska/matroska-demux.c"

perl -0pi -e 's/qtdemux->dts_support = FALSE;/qtdemux->dts_support = TRUE;/g' "$QTDEMUX"
perl -0pi -e 's/demux->dts_support = FALSE;/demux->dts_support = TRUE;/g'     "$TSDEMUX"
# matroska: with dts_support FALSE this build drops A_DTS tracks entirely (no
# pad), unlike LG's stock binary, which re-tags them for our dtsdec.
perl -0pi -e 's/demux->dts_support = FALSE;/demux->dts_support = TRUE;/g'     "$MKVDEMUX"

echo "=== DTS patch verification ==="
for f in "$QTDEMUX" "$TSDEMUX" "$MKVDEMUX"; do
  echo "--- $f"
  grep -n 'dts_support = TRUE'  "$f" || { echo "PATCH FAILED: no TRUE in $f"; exit 1; }
  if grep -n 'dts_support = FALSE' "$f"; then
    echo "PATCH FAILED: dts_support = FALSE still present in $f"; exit 1
  fi
done
echo "=== DTS patch OK (all three files: dts_support = TRUE, no remaining FALSE) ==="

# ---------------------------------------------------------------------------
# TRUEHD-IN-MPEG-TS PATCH: LG wraps the BD TrueHD stream-type case in tsdemux.c
# in `#if 0` and falls through to `goto done`, so stream_type 0x83
# (ST_BD_AUDIO_AC3_TRUE_HD) is silently DROPPED -- the pad is never exposed and
# what actually decodes is the AC-3 compatibility substream carried on the same
# BD PID. That is why TrueHD in .ts/.m2ts "plays fine" but is not TrueHD.
#
# LG's own comment gates it on "until we have ability to decode this codec";
# this payload ships avdec_truehd (ranked 310 by install.sh), so that
# precondition now holds. Un-#if-0 the case so the pad is exposed. The
# `stream->target_pes_substream = 0x72` inside it is load-bearing: it selects
# the TrueHD PES substream rather than the embedded AC-3 core.
#
# Applied to the copied source, then verified (build fails if it did not land).
# ---------------------------------------------------------------------------
perl - "$TSDEMUX" <<'TRUEHD_PL'
use strict; use warnings;
local $/; my $f = shift; open my $fh, '<', $f or die "$f: $!"; my $s = <$fh>; close $fh;
my $before = $s;
$s =~ s{
      \ {6}case\ ST_BD_AUDIO_AC3_TRUE_HD:\n
      \ {8}/\*\ FIXME\ :\ Do\ not\ expose\ pad\ of\ trueHD\ codec\ until\ we\ have\n
      \ {9}\*\ ability\ to\ decode\ this\ codec\.\ \*/\n
      \#if\ 0\n
      (.*?)
      \ {8}break;\n
      \#endif\n
      \ {8}goto\ done;\n
}{      case ST_BD_AUDIO_AC3_TRUE_HD:\n        /* dts_restore_plus: LG gated this behind #if 0 with "do not expose pad\n         * of trueHD codec until we have ability to decode this codec" -- this\n         * payload ships avdec_truehd, so the pad IS exposed here. The\n         * target_pes_substream = 0x72 selects the TrueHD PES substream instead\n         * of the AC-3 core embedded on the same BD PID. */\n$1        break;\n}xs;
die "TRUEHD PATCH FAILED: anchor block not matched in $f\n" if $s eq $before;
open my $out, '>', $f or die "$f: $!"; print $out $s; close $out;
TRUEHD_PL

echo "=== TrueHD patch verification ==="
grep -n 'audio/x-true-hd' "$TSDEMUX" \
  || { echo "PATCH FAILED: no audio/x-true-hd caps in $TSDEMUX"; exit 1; }
grep -n 'target_pes_substream = 0x72' "$TSDEMUX" \
  || { echo "PATCH FAILED: TrueHD PES substream 0x72 not set in $TSDEMUX"; exit 1; }
if grep -n 'FIXME : Do not expose pad of trueHD' "$TSDEMUX"; then
  echo "PATCH FAILED: TrueHD case still gated in $TSDEMUX"; exit 1
fi
# The removed block took exactly one #if/#endif pair with it; assert balance so a
# future source change cannot silently leave the preprocessor lopsided.
TH_IF=$(grep -c '^[[:space:]]*#if ' "$TSDEMUX" || true)
TH_IFDEF=$(grep -c '^[[:space:]]*#ifdef' "$TSDEMUX" || true)
TH_IFNDEF=$(grep -c '^[[:space:]]*#ifndef' "$TSDEMUX" || true)
TH_ENDIF=$(grep -c '^[[:space:]]*#endif' "$TSDEMUX" || true)
if [ $((TH_IF + TH_IFDEF + TH_IFNDEF)) -ne "$TH_ENDIF" ]; then
  echo "PATCH FAILED: preprocessor unbalanced in $TSDEMUX" \
       "(#if=$TH_IF #ifdef=$TH_IFDEF #ifndef=$TH_IFNDEF vs #endif=$TH_ENDIF)"; exit 1
fi
echo "=== TrueHD patch OK (audio/x-true-hd exposed, substream 0x72, balanced) ==="

# ---------------------------------------------------------------------------
# DTS-HD-IN-MPEG-TS PATCH: for the BD DTS stream types, tsdemux keeps only PES
# substream 0x71 (the DTS core) and drops 0x72, the DTS-HD extension substream
# that carries XLL/XBR/X96. dtsdec now decodes the extension (FFmpeg dca), so
# both substreams are passed on; dtsdec's parser joins core + extension back
# into one frame.
# ---------------------------------------------------------------------------
perl - "$TSDEMUX" <<'DTSHD_PL'
use strict; use warnings;
local $/; my $f = shift; open my $fh, '<', $f or die "$f: $!"; my $s = <$fh>; close $fh;
my $n = ($s =~ s{
      (caps\ =\ gst_caps_new_empty_simple\ \("audio/x-dts"\);\n)
      \ {10}stream->target_pes_substream\ =\ 0x71;\n
}{$1}xs);
die "DTS-HD PATCH FAILED: expected 1 core-only substream filter in $f, got $n\n" if $n != 1;
open my $out, '>', $f or die "$f: $!"; print $out $s; close $out;
DTSHD_PL
if grep -n 'target_pes_substream = 0x71' "$TSDEMUX"; then
  echo "PATCH FAILED: DTS core-only substream filter still present in $TSDEMUX"; exit 1
fi
echo "=== DTS-HD patch OK (both BD DTS substreams passed on) ==="

# ---------------------------------------------------------------------------
# DOLBY VISION PROFILE 7 (MKV) PATCHES -- two changes to matroska-demux.c:
#
# 1. LG returns early for dv_profile == 7 ("not supported, but can play as
#    HDR10"), so UHD-BD P7 MKVs never get Dolby Vision caps. Fold P7 into the
#    "< 7 requires a dvcC box" branch so it reaches "Set as Dolby Vision". The
#    TV decodes the base layer + RPU; a FEL residual layer is not decoded.
#
# 2. The BlockAdditionMapping parser treats EVERY BlockAddIDExtraData as a DOVI
#    config. UHD-BD P7 MKVs carry a second mapping of type hvcE (the EL's HEVC
#    config), whose bytes then overwrite dv_profile (read as 16) and kill
#    playback. Remember each mapping's BlockAddIDType and only parse extradata
#    from a dvcC (1685480259) or dvvC (1685485123) mapping.
#
# Shipped as a source build, not a byte-patched stock binary (the FEL green
# tint first blamed on the byte patch also showed with this build, on a TV
# degraded by hours of uptime; neither showed it after a reboot). Applied to
# the copied source, then verified (build fails if either change did not land).
# ---------------------------------------------------------------------------
perl - "$MKVDEMUX" <<'DV7_PL'
use strict; use warnings;
local $/; my $f = shift; open my $fh, '<', $f or die "$f: $!"; my $s = <$fh>; close $fh;
my $n;
$n = $s =~ s~if \(demux->dv_profile == 7\) \{\n\s*GST_DEBUG_OBJECT \(demux,\n\s*"Dolby Vision profile 7 is not supported, but can play as HDR10\."\);\n\s*return TRUE;\n\s*\} else if \(demux->dv_profile < 7\) \{~if (demux->dv_profile <= 7) {~g;
die "DV7 PATCH FAILED: P7 HDR10 gate matched ${\($n||0)} times in $f\n" unless $n && $n == 1;
$n = $s =~ s~(      case GST_MATROSKA_ID_BLOCKADDITIONMAPPING:\{\n)~$1        guint64 map_type = 0;\n~g;
die "DV7 PATCH FAILED: BlockAdditionMapping case matched ${\($n||0)} times in $f\n" unless $n && $n == 1;
$n = $s =~ s~("BlockAdditionMapping BlockAddIDType: %" G_GUINT64_FORMAT,\n\s*num\);\n)~$1              map_type = num;\n~g;
die "DV7 PATCH FAILED: BlockAddIDType debug matched ${\($n||0)} times in $f\n" unless $n && $n == 1;
$n = $s =~ s~(&size\)\) != GST_FLOW_OK\)\n\s*break;\n\n)( *)(demux->dv_profile = \(data\[2\] >> 1\) & 0x7f;)~$1$2/* dts_restore_plus: only dvcC/dvvC carry a DOVI config (hvcE is the EL's HEVC config) */\n$2if (map_type != 1685480259 && map_type != 1685485123) {\n$2  g_free (data);\n$2  break;\n$2}\n\n$2$3~g;
die "DV7 PATCH FAILED: DOVI extradata parse matched ${\($n||0)} times in $f\n" unless $n && $n == 1;
open my $out, '>', $f or die "$f: $!"; print $out $s; close $out;
DV7_PL

echo "=== DV7 patch verification ==="
grep -n 'demux->dv_profile <= 7' "$MKVDEMUX" \
  || { echo "PATCH FAILED: no dv_profile <= 7 in $MKVDEMUX"; exit 1; }
if grep -n 'profile 7 is not supported' "$MKVDEMUX"; then
  echo "PATCH FAILED: DV7 HDR10 gate still present in $MKVDEMUX"; exit 1
fi
grep -n 'map_type = num;' "$MKVDEMUX" \
  || { echo "PATCH FAILED: BlockAddIDType not recorded in $MKVDEMUX"; exit 1; }
grep -n 'map_type != 1685480259 && map_type != 1685485123' "$MKVDEMUX" \
  || { echo "PATCH FAILED: extradata not gated on dvcC/dvvC in $MKVDEMUX"; exit 1; }
echo "=== DV7 patch OK (P7 -> Dolby Vision, extradata gated on dvcC/dvvC) ==="

# ---------------------------------------------------------------------------
# DOLBY VISION PROFILE 7 (MP4) PATCH -- qtdemux.c has the same early return for
# dv_profile == 7 as matroska-demux.c (change 1 above); fold P7 into the "< 7
# requires a dvcC box" branch the same way. qtdemux reads the DOVI config only
# from the dvcC/dvvC box itself, so change 2 has no MP4 counterpart.
# ---------------------------------------------------------------------------
perl - "$QTDEMUX" <<'DV7MP4_PL'
use strict; use warnings;
local $/; my $f = shift; open my $fh, '<', $f or die "$f: $!"; my $s = <$fh>; close $fh;
my $n = ($s =~ s{
      if\ \(qtdemux->dv_profile\ ==\ 7\)\ \{\n
      \s*GST_DEBUG_OBJECT\ \(qtdemux,\n
      \s*"Dolby\ Vision\ profile\ 7\ is\ not\ supported,\ but\ can\ play\ as\ HDR10\."\);\n
      \s*return\ TRUE;\n
      \s*\}\ else\ if\ \(qtdemux->dv_profile\ <\ 7\)\ \{
}{if (qtdemux->dv_profile <= 7) \{}xs);
die "DV7 MP4 PATCH FAILED: expected 1 P7 gate in $f, got $n\n" if $n != 1;
open my $out, '>', $f or die "$f: $!"; print $out $s; close $out;
DV7MP4_PL
grep -n 'qtdemux->dv_profile <= 7' "$QTDEMUX" \
  || { echo "PATCH FAILED: no dv_profile <= 7 in $QTDEMUX"; exit 1; }
if grep -n 'profile 7 is not supported' "$QTDEMUX"; then
  echo "PATCH FAILED: DV7 HDR10 gate still present in $QTDEMUX"; exit 1
fi
echo "=== DV7 MP4 patch OK (P7 -> Dolby Vision in qtdemux) ==="

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
grep -n 'demux->dts_support = TRUE'   "$SRC/gst-plugins-good/gst/matroska/matroska-demux.c"
grep -n 'demux->dv_profile <= 7'      "$SRC/gst-plugins-good/gst/matroska/matroska-demux.c"

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
  -Dauto_features=disabled -Disomp4=enabled -Dmatroska=enabled -Dbz2=enabled \
  -Ddca=true -Dorc=disabled
ninja -C "$WORK/good" install

meson setup "$WORK/bad" "$SRC/gst-plugins-bad" $COMMON \
  -Dintrospection=disabled \
  -Dauto_features=disabled -Dmpegtsdemux=enabled -Ddca=true -Dorc=disabled
ninja -C "$WORK/bad" install

cp "$PREFIX/lib/gstreamer-1.0/libgstisomp4.so" "$OUT/"
cp "$PREFIX/lib/gstreamer-1.0/libgstmpegtsdemux.so" "$OUT/"
cp "$PREFIX/lib/gstreamer-1.0/libgstmatroska.so" "$OUT/"
arm-linux-gnueabi-strip --strip-unneeded "$OUT/libgstisomp4.so" "$OUT/libgstmpegtsdemux.so" \
  "$OUT/libgstmatroska.so"
# Debian's bz2 soname is libbz2.so.1.0; the C5 only has libbz2.so.1 (the stock
# libgstmatroska.so NEEDs that name too), so rewrite it or the plugin won't load.
patchelf --replace-needed libbz2.so.1.0 libbz2.so.1 "$OUT/libgstmatroska.so"
arm-linux-gnueabi-readelf -d "$OUT/libgstmatroska.so" | grep -q 'NEEDED.*\[libbz2\.so\.1\]' \
  || { echo "libgstmatroska.so does not NEED libbz2.so.1"; exit 1; }

for so in "$OUT"/libgstisomp4.so "$OUT"/libgstmpegtsdemux.so "$OUT"/libgstmatroska.so; do
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
      libglib2.0-dev-bin libglib2.0-dev:armel zlib1g-dev:armel libbz2-dev:armel \\
      file binutils && \\
    rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir 'meson==1.4.2' 'patchelf==0.19.1.0'
COPY cross-armel.txt /cross-armel.txt
COPY build-inside.sh /build-inside.sh
RUN chmod +x /build-inside.sh
EOF

docker build --build-arg SNAPSHOT=$SNAPSHOT -t demux-armel "$CTX"
docker run --rm -v "$CTX/src":/src:ro -v "$OUT":/out demux-armel /build-inside.sh
echo "Artifacts in $OUT"
