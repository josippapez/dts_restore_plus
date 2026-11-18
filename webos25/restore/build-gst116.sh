#!/usr/bin/env bash
#
# build-gst116.sh — Cross-build the webOS 6 / GStreamer 1.16 `dtsdec` plugin
# for LG OLED C1/G1 (profile webos6-o20n-gst116-spdif) and test it under qemu.
#
# Target ABI (report-tv.sh on an OLED65C17LB, firmware 03.53.45):
#   ELF32 ARM, EABI5 soft-float (e_flags 0x05000200), loader ld-linux.so.3,
#   aarch64 kernel 4.4.84 with a 32-bit userspace, GStreamer 1.16.2.
#
# Toolchain choice:
#   - Ubuntu 18.04's arm-linux-gnueabi cross toolchain (GCC 7.5, glibc 2.27):
#     an old sysroot, so the plugin cannot reference a GLIBC_* symbol version
#     newer than 2.27. Newer cross toolchains (Ubuntu 24.04, Debian 12+) also
#     default 32-bit ARM to 64-bit time_t and pull GLIBC_2.34 time64 symbols.
#   - GStreamer 1.16.2 core + base and glib 2.58.3 are built from source, so
#     the plugin is compiled against the exact 1.16.2 headers and its
#     GST_PLUGIN_DEFINE version (1.16) matches the TV's core. A 1.18+ header
#     set would make the TV's 1.16 core reject the plugin.
#   - FFmpeg n4.4.4, dca decoder only, static and --exclude-libs (no exported
#     av_* symbols), the same tag and soft-float no-asm settings as build.sh.
#
# Only the Ubuntu apt archive (plain HTTP) is used inside Docker. All sources
# are cloned on the host at pinned commits into .gst116-src/ (git-ignored) and
# copied into the image, so the build needs no network access from containers.
#
# Output in webos25/restore/gst116-out/:
#   libgstdtsdec.so     the plugin (passthrough to S/PDIF + stereo decode)
#   BUILD-REPORT.txt    ELF header, NEEDED, max GLIBC, exports, qemu results
#
# Usage: ./build-gst116.sh            build + checks + qemu tests
#        SKIP_TESTS=1 ./build-gst116.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/gst116-out}"
CACHE="${CACHE:-$HERE/.gst116-src}"
IMAGE=dts-gst116-build
# The plugin must not need a newer glibc than this. The sysroot is 2.27; the
# plugin itself is expected to need only GLIBC_2.4/2.7 symbols.
GLIBC_CEILING="${GLIBC_CEILING:-2.17}"
mkdir -p "$OUT" "$CACHE"

# --- 1. sources at pinned commits (host side) ---------------------------------
# name  repository                                tag        commit
PINS='
glib              https://github.com/GNOME/glib.git                 2.58.3  f2d1128ffae0de87680e411fb62ccfccafed6b1c
gstreamer         https://github.com/GStreamer/gstreamer.git        1.16.2  129493687793cbc109d6211bb0e465218e383e9d
gst-plugins-base  https://github.com/GStreamer/gst-plugins-base.git 1.16.2  9d3581b2e6f12f0b7e790d1ebb63b90cf5b1ef4e
gst-plugins-good  https://github.com/GStreamer/gst-plugins-good.git 1.16.2  ce0723527aa37d5f4d19ef8021c0b2eb8f83b08d
FFmpeg            https://github.com/FFmpeg/FFmpeg.git              n4.4.4  71fb6132637a2a430375c24afc381fff8b854fe7
zlib              https://github.com/madler/zlib.git                v1.3.1  51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
libffi            https://github.com/libffi/libffi.git              v3.3    5dcb741f1544c5e18c9dbf96aeb8b61cc556a616
meson             https://github.com/mesonbuild/meson.git           0.53.2  156e066199bcc44523edec43b4f98a56a5de3a29
'
echo "$PINS" | while read -r name url tag commit; do
  [ -n "$name" ] || continue
  if [ ! -d "$CACHE/$name/.git" ]; then
    echo "--- fetching $name $tag"
    git -c advice.detachedHead=false clone -q --depth 1 -b "$tag" "$url" "$CACHE/$name"
  fi
  got=$(git -C "$CACHE/$name" rev-parse HEAD)
  if [ "$got" != "$commit" ]; then
    echo "ERROR: $name $tag is $got, expected $commit" >&2
    exit 1
  fi
done

# --- 2. toolchain + 1.16.2 sysroot + static FFmpeg (cached image) -------------
docker build -t "$IMAGE" -f - "$CACHE" <<'DOCKERFILE_EOF'
FROM ubuntu:18.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      gcc g++ gcc-arm-linux-gnueabi g++-arm-linux-gnueabi libc6-dev-armel-cross \
      binutils-arm-linux-gnueabi python3 python3-setuptools ninja-build pkg-config make \
      autoconf automake libtool libltdl-dev texinfo bison flex gettext qemu-user file \
 && rm -rf /var/lib/apt/lists/*

ENV P=/opt/sr
ENV PKG_CONFIG_LIBDIR=/opt/sr/lib/pkgconfig
ENV ARMFLAGS="-march=armv7-a -mfloat-abi=soft -O2"
RUN printf '%s\n' "[binaries]" "c = 'arm-linux-gnueabi-gcc'" \
      "cpp = 'arm-linux-gnueabi-g++'" "ar = 'arm-linux-gnueabi-ar'" \
      "strip = 'arm-linux-gnueabi-strip'" "pkgconfig = 'pkg-config'" \
      "exe_wrapper = ['qemu-arm', '-L', '/usr/arm-linux-gnueabi']" \
      "[properties]" "needs_exe_wrapper = true" \
      "c_args = ['-march=armv7-a', '-mfloat-abi=soft', '-O2']" \
      "[host_machine]" "system = 'linux'" "cpu_family = 'arm'" \
      "cpu = 'armv7'" "endian = 'little'" > /opt/armel.cross
ENV MESON="python3 /src/meson/meson.py"
# q: run a build step quietly, but show the tail of its log if it fails.
RUN printf '%s\n' '#!/bin/sh' 'L=$(mktemp)' '"$@" >"$L" 2>&1 && exit 0' \
      'r=$?; tail -n 40 "$L"; exit $r' > /usr/local/bin/q && chmod +x /usr/local/bin/q

COPY zlib /src/zlib
RUN cd /src/zlib && CC=arm-linux-gnueabi-gcc AR=arm-linux-gnueabi-ar \
      RANLIB=arm-linux-gnueabi-ranlib CFLAGS="$ARMFLAGS -fPIC" \
      ./configure --prefix=$P >/dev/null && make -j"$(nproc)" install >/dev/null

COPY libffi /src/libffi
RUN cd /src/libffi && ./autogen.sh >/dev/null 2>&1 \
 && ./configure --host=arm-linux-gnueabi --prefix=$P --libdir=$P/lib \
      --disable-docs CFLAGS="$ARMFLAGS" >/dev/null \
 && make -j"$(nproc)" >/dev/null 2>&1 && make install >/dev/null 2>&1 \
 && test -f $P/lib/libffi.so

COPY meson /src/meson
COPY glib /src/glib
RUN cd /src/glib && rm -rf subprojects/libffi \
 && q $MESON b --cross-file /opt/armel.cross --prefix=$P --libdir=lib \
      --buildtype=release -Dlibmount=false -Dselinux=false -Dxattr=false \
      -Dinternal_pcre=true -Dman=false -Dgtk_doc=false -Dfam=false \
      -Diconv=libc \
 && q ninja -C b install

# glib-mkenums/glib-genmarshal are arch-independent Python scripts.
ENV PATH=/opt/sr/bin:$PATH
COPY gstreamer /src/gstreamer
RUN cd /src/gstreamer && q $MESON b --cross-file /opt/armel.cross --prefix=$P \
      --libdir=lib --buildtype=release -Dtests=disabled -Dexamples=disabled \
      -Dbenchmarks=disabled -Dgtk_doc=disabled -Dintrospection=disabled \
      -Dlibunwind=disabled -Dlibdw=disabled -Dbash-completion=disabled \
      -Dcheck=disabled -Dnls=disabled \
 && q ninja -C b install

COPY gst-plugins-base /src/gst-plugins-base
RUN cd /src/gst-plugins-base && q $MESON b --cross-file /opt/armel.cross \
      --prefix=$P --libdir=lib --buildtype=release -Dauto_features=disabled \
      -Dtests=disabled -Dexamples=disabled -Dgl=disabled \
      -Dintrospection=disabled -Dorc=disabled -Dnls=disabled -Dtools=disabled \
      -Dgtk_doc=disabled -Daudioconvert=enabled -Dplayback=enabled \
      -Dtypefind=enabled \
 && q ninja -C b install

# Test-only: qtdemux + matroskademux 1.16.2, to feed the plugin under qemu
# exactly as LG's upstream-derived demuxers would.
COPY gst-plugins-good /src/gst-plugins-good
RUN cd /src/gst-plugins-good && q $MESON b --cross-file /opt/armel.cross \
      --prefix=$P --libdir=lib --buildtype=release -Dauto_features=disabled \
      -Disomp4=enabled -Dmatroska=enabled -Daudioparsers=enabled -Dtests=disabled -Dexamples=disabled \
      -Dorc=disabled -Dnls=disabled -Dgtk_doc=disabled \
 && q ninja -C b install

# Static FFmpeg: dca decoder only. The configure line, prefix included, is
# embedded in the binary: keep /opt/ffdca.
COPY FFmpeg /src/FFmpeg
RUN cd /src/FFmpeg && ./configure --cross-prefix=arm-linux-gnueabi- \
      --enable-cross-compile --arch=arm --cpu=armv7-a --target-os=linux \
      --prefix=/opt/ffdca --extra-cflags="-mfloat-abi=soft" \
      --disable-everything --enable-decoder=dca --disable-avformat \
      --disable-avfilter --disable-swresample --disable-swscale \
      --disable-avdevice --disable-postproc --disable-network \
      --disable-programs --disable-doc --disable-autodetect \
      --disable-pthreads --disable-asm \
      --enable-static --disable-shared --enable-pic >/dev/null \
 && { grep -q "CONFIG_DCA_DECODER 1" config.h \
      || { echo "ERROR: FFmpeg configured without the dca decoder"; exit 1; }; } \
 && make -j"$(nproc)" install >/dev/null \
 && rm -rf /src/FFmpeg
DOCKERFILE_EOF

# --- 3. compile + ELF checks + qemu tests -------------------------------------
docker run --rm -i \
  -e GLIBC_CEILING="$GLIBC_CEILING" -e SKIP_TESTS="${SKIP_TESTS:-}" \
  -v "$HERE/src/gst116":/work:ro \
  -v "$HERE/testfiles":/testfiles:ro \
  -v "$OUT":/out \
  "$IMAGE" /bin/bash -euo pipefail -s <<'CONTAINER_EOF'
R=/out/BUILD-REPORT.txt
exec > >(tee "$R") 2>&1
echo "=== webos6-o20n-gst116-spdif dtsdec build ==="
echo "toolchain: $(arm-linux-gnueabi-gcc --version | head -1)"
echo "sysroot glibc: $(ls /usr/arm-linux-gnueabi/lib/libc-*.so | sed 's/.*libc-//; s/\.so//')"
echo "gstreamer headers: $(pkg-config --modversion gstreamer-1.0) / base $(pkg-config --modversion gstreamer-audio-1.0)"
echo "glib headers: $(pkg-config --modversion glib-2.0)"

CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)
LB=$(pkg-config --libs gstreamer-1.0 gstreamer-audio-1.0 gstreamer-base-1.0)
# GLIB_VERSION_MAX_ALLOWED keeps the plugin to glib API that every webOS 6
# glib has; the TV's glib is newer than 2.40, never older.
arm-linux-gnueabi-gcc -march=armv7-a -mfloat-abi=soft -shared -fPIC -O2 \
  -Wall -Wextra -Wno-unused-parameter -Werror \
  -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40 \
  -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_40 -DGST_DISABLE_DEPRECATED \
  -DVERSION='"1.16.2-webos6dts"' -DPACKAGE='"gst-plugins-bad"' \
  -DGST_PACKAGE_NAME='"WebOS DTS restore (webOS 6)"' \
  -DGST_PACKAGE_ORIGIN='"https://github.com/josippapez/dts_restore_plus"' \
  -I/opt/ffdca/include $CF \
  -o /out/libgstdtsdec.so /work/gstdtsdec116.c \
  /opt/ffdca/lib/libavcodec.a /opt/ffdca/lib/libavutil.a $LB -lm \
  -Wl,--exclude-libs,ALL -Wl,--no-undefined -Wl,--as-needed

so=/out/libgstdtsdec.so
echo; echo "=== ELF header (must be ARM, 0x5000200 soft-float) ==="
arm-linux-gnueabi-readelf -h $so | grep -E "Class|Machine|Flags"
flags=$(arm-linux-gnueabi-readelf -h $so | sed -n 's/.*Flags: *\(0x[0-9a-f]*\).*/\1/p')
[ "$flags" = 0x5000200 ] || { echo "ERROR: e_flags $flags"; exit 1; }
echo; echo "=== dynamic section (no RPATH/RUNPATH) ==="
arm-linux-gnueabi-readelf -d $so | grep -E "NEEDED|RPATH|RUNPATH|SONAME" || true
if arm-linux-gnueabi-readelf -d $so | grep -qE "RPATH|RUNPATH"; then echo "ERROR: rpath"; exit 1; fi
echo; echo "=== GLIBC symbol versions (max must be <= $GLIBC_CEILING) ==="
arm-linux-gnueabi-objdump -T $so | grep -oE "GLIBC_[0-9.]+" | sort -V | uniq -c
max=$(arm-linux-gnueabi-objdump -T $so | grep -oE "GLIBC_[0-9.]+" | sed 's/GLIBC_//' | sort -V | tail -1)
echo "max GLIBC_$max"
[ "$(printf '%s\n%s\n' "$max" "$GLIBC_CEILING" | sort -V | tail -1)" = "$GLIBC_CEILING" ] \
  || { echo "ERROR: needs GLIBC_$max > $GLIBC_CEILING"; exit 1; }
echo; echo "=== exported symbols (plugin entry points only; 0 FFmpeg) ==="
arm-linux-gnueabi-nm -D --defined-only $so | awk '{print $3}' | sort
n=$(arm-linux-gnueabi-nm -D --defined-only $so | grep -cE " (av|ff|avpriv|avcodec|swr)_" || true)
[ "$n" = 0 ] || { echo "ERROR: $n FFmpeg symbols exported"; exit 1; }
arm-linux-gnueabi-nm -D --defined-only $so | grep -q " gst_plugin_dtsdec_get_desc$" \
  || { echo "ERROR: gst_plugin_dtsdec_get_desc not exported"; exit 1; }
echo; echo "=== GStreamer/glib imports (all must exist in 1.16.2 / glib 2.40+) ==="
arm-linux-gnueabi-nm -D --undefined-only $so | awk '{print $2}' | grep -E "^(gst|g)_" | sort | tr '\n' ' '; echo
echo; sha256sum $so; ls -l $so

[ -z "$SKIP_TESTS" ] || exit 0

# --- qemu tests on the real 1.16.2 core ---------------------------------------
echo; echo "=== qemu-arm tests (GStreamer $(pkg-config --modversion gstreamer-1.0), soft-float) ==="
export QEMU_LD_PREFIX=/usr/arm-linux-gnueabi
export LD_LIBRARY_PATH=/opt/sr/lib
export GST_PLUGIN_SYSTEM_PATH=/opt/sr/lib/gstreamer-1.0
export GST_PLUGIN_PATH=/out
export GST_REGISTRY=/tmp/registry.bin GST_REGISTRY_FORK=no
# Any GLib/GStreamer CRITICAL or WARNING is a test failure, not noise.
export G_DEBUG=fatal-criticals
GL() { qemu-arm /opt/sr/bin/gst-launch-1.0 -q "$@" 2>/tmp/gl.err || { tail -n 5 /tmp/gl.err; return 1; }
       ! grep -qE "CRITICAL|WARNING" /tmp/gl.err || { head -n 5 /tmp/gl.err; return 1; }; }
INSP() { qemu-arm /opt/sr/bin/gst-inspect-1.0 "$@"; }
fail() { echo "FAIL: $*"; exit 1; }

INSP dtsdec > /tmp/inspect.txt || fail "gst-inspect could not load the plugin"
grep -E "Version|Rank|Long-name|Klass" /tmp/inspect.txt
grep -q "audio/x-dts" /tmp/inspect.txt || fail "sink caps"

MP4=/testfiles/DTS-in-mp4.mp4
# FFmpeg n4.4.4 `-c copy -f spdif` output of the same track (the exact form
# that made a C1's optical receiver show DTS); 348 bursts of 2048 bytes.
REF=5cdce24f2e4ca0cc72253786e40094e7cd9d5f23b6f6b2c42658d006bf4c3c33

echo "--- 1. qtdemux -> dtsdec output=passthrough: bit-exact against FFmpeg spdif"
GL filesrc location=$MP4 ! qtdemux ! dtsdec output=passthrough \
  ! "audio/x-raw,format=S16LE,rate=48000,channels=2" ! filesink location=/tmp/pt.raw
got=$(sha256sum /tmp/pt.raw | cut -d' ' -f1); echo "$(stat -c%s /tmp/pt.raw) bytes $got"
[ "$got" = "$REF" ] || fail "passthrough output differs from FFmpeg's spdif muxer"

echo "--- 2. unframed elementary DTS through the plugin's own parser"
GL filesrc location=$MP4 ! qtdemux name=d d.audio_0 ! filesink location=/tmp/es.dts
GL filesrc location=/tmp/es.dts blocksize=1000 ! "audio/x-dts" ! dtsdec output=passthrough \
  ! filesink location=/tmp/pt2.raw
[ "$(sha256sum /tmp/pt2.raw | cut -d' ' -f1)" = "$REF" ] || fail "parser path differs"
echo "identical"

echo "--- 3. matroskademux (1.16) -> dtsdec: the C1's stock MKV path"
GL filesrc location=$MP4 ! qtdemux ! dcaparse ! matroskamux ! filesink location=/tmp/t.mkv
GL filesrc location=/tmp/t.mkv ! matroskademux ! dtsdec output=passthrough \
  ! filesink location=/tmp/pt3.raw
[ "$(sha256sum /tmp/pt3.raw | cut -d' ' -f1)" = "$REF" ] || fail "MKV path differs"
echo "identical"

echo "--- 4. decode fallback: stereo S16LE, full length, not silent"
GL filesrc location=$MP4 ! qtdemux ! dtsdec output=decode \
  ! "audio/x-raw,format=S16LE,channels=2" ! filesink location=/tmp/dec.raw
bytes=$(stat -c%s /tmp/dec.raw); echo "$bytes bytes"
[ "$bytes" = "$(stat -c%s /tmp/pt.raw)" ] || fail "decoded length != stream length"
od -An -v -td2 -w2 /tmp/dec.raw | awk '{v=$1<0?-$1:$1; if(v>m)m=v} END{print "peak", m; exit !(m>1000)}' \
  || fail "decoded output is silent"

echo "--- 5. auto mode follows the opt-in flag and the live watcher"
mkdir -p /var/lib/webosbrew/dtsc1 /run/dtsc1
decide() { GL filesrc location=$MP4 ! qtdemux ! dtsdec ! fakesink; cut -d' ' -f2- /run/dtsc1/last-decision; }
rm -f /var/lib/webosbrew/dtsc1/passthrough /run/dtsc1/sound-output
d=$(decide); echo "no opt-in      -> $d"; case $d in decode*) ;; *) fail "$d";; esac
touch /var/lib/webosbrew/dtsc1/passthrough
d=$(decide); echo "no watcher     -> $d"; case $d in decode*) ;; *) fail "$d";; esac
sleep 600 & W=$!
echo "$W external_optical passThrough" > /run/dtsc1/sound-output
d=$(decide); echo "optical+PT     -> $d"; case $d in passthrough*) ;; *) fail "$d";; esac
echo "$W tv_speaker passThrough" > /run/dtsc1/sound-output
d=$(decide); echo "tv speakers    -> $d"; case $d in decode*) ;; *) fail "$d";; esac
echo "$W external_optical auto" > /run/dtsc1/sound-output
d=$(decide); echo "optical+Auto   -> $d"; case $d in decode*) ;; *) fail "$d";; esac
echo "$W external_optical passThrough" > /run/dtsc1/sound-output; kill $W; wait $W 2>/dev/null || true
d=$(decide); echo "dead watcher   -> $d"; case $d in decode*) ;; *) fail "$d";; esac

echo "--- 6. decodebin autoplugs dtsdec by rank (how a player finds it)"
rm -f /run/dtsc1/last-decision
GL filesrc location=$MP4 ! decodebin ! fakesink
[ -f /run/dtsc1/last-decision ] || fail "decodebin did not instantiate dtsdec"
d=$(cut -d' ' -f2- /run/dtsc1/last-decision); echo "decodebin picked dtsdec: $d"
case $d in decode*|passthrough*) ;; *) fail "decodebin did not use dtsdec";; esac

echo; echo "ALL QEMU TESTS PASSED"
CONTAINER_EOF

ls -la "$OUT"
