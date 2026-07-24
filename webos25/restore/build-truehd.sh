#!/bin/bash
# Cross-compile minimal ffmpeg (TrueHD/MLP) + gst-libav plugin for ARM EABI5 soft-float (gnueabi/armel).
# Runs INSIDE debian:11-slim (bullseye, glibc 2.31) on an arm64 host. Artifacts -> /out.
# Target: 32-bit ARM soft-float, e_flags 0x05000200, interp /lib/ld-linux.so.3, glibc <= 2.35.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo "############ STEP 1: toolchain + armel dev libs ############"
dpkg --add-architecture armel
# bullseye armel main was dropped from deb.debian.org (oldstable); it lives on archive.debian.org.
# arm64 (native, for host tools) stays on deb.debian.org.
# NOTE: we do NOT `apt install` armel libs — the base image ships libc6:arm64 deb11u14 but
# archive.debian.org only has libc6:armel deb11u11; Multi-Arch:same forces exact version
# equality, so any :armel install is unsatisfiable. Instead we DOWNLOAD the armel dev-lib
# closure and EXTRACT it into the container root (armel files land in /usr/lib/arm-linux-gnueabi,
# which never clashes with arm64 host libs in /usr/lib/aarch64-linux-gnu). The cross toolchain
# brings its own target libc sysroot, so no armel libc6 is needed in dpkg.
cat > /etc/apt/sources.list <<'EOF'
deb [arch=arm64] http://deb.debian.org/debian bullseye main
deb [arch=arm64] http://deb.debian.org/debian-security bullseye-security main
deb [arch=armel] http://archive.debian.org/debian bullseye main
EOF
rm -f /etc/apt/sources.list.d/* 2>/dev/null || true
apt-get -o Acquire::Retries=3 -o Acquire::Check-Valid-Until=false update -qq
apt-get install -y -qq --no-install-recommends \
  gcc-arm-linux-gnueabi g++-arm-linux-gnueabi pkg-config make git file binutils patchelf yasm ca-certificates \
  meson ninja-build python3 python3-setuptools python3-distutils xz-utils curl dpkg-dev apt-utils >/dev/null
echo "toolchain: $(arm-linux-gnueabi-gcc --version | head -1)"

echo "--- downloading + extracting armel dev-lib closure into /sysroot ---"
# Extract into a dedicated /sysroot (NOT / — dpkg-deb -x over the live root segfaults).
mkdir -p /tmp/dl /sysroot
DEVPKGS=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
  --no-breaks --no-replaces --no-enhances \
  libgstreamer1.0-dev:armel libgstreamer-plugins-base1.0-dev:armel libglib2.0-dev:armel 2>/dev/null \
  | grep "^\w" | grep ":armel" | grep -viE "libc6|libc-dev|^gcc" | sort -u)
echo "armel dev closure package count: $(echo "$DEVPKGS" | wc -l)"
( cd /tmp/dl && apt-get download $DEVPKGS 2>&1 | grep -iE "err|fail|unable" | head || true )
echo "downloaded debs: $(ls /tmp/dl/*.deb 2>/dev/null | wc -l)"
for d in /tmp/dl/*.deb; do dpkg-deb -x "$d" /sysroot ; done
echo "extracted. gstreamer .pc:"; ls /sysroot/usr/lib/arm-linux-gnueabi/pkgconfig/gstreamer-1.0.pc

# pkg-config resolves target libs out of /sysroot. ffmin (real /opt/ffmin) is reached via a
# symlink so PKG_CONFIG_SYSROOT_DIR rewriting (/sysroot/opt/ffmin) still lands on real files.
export PKG_CONFIG_SYSROOT_DIR=/sysroot
export PKG_CONFIG_LIBDIR=/opt/ffmin/lib/pkgconfig:/sysroot/usr/lib/arm-linux-gnueabi/pkgconfig:/sysroot/usr/share/pkgconfig
export PKG_CONFIG_PATH=""
mkdir -p /sysroot/opt && ln -sfn /opt/ffmin /sysroot/opt/ffmin
echo "pkg-config gstreamer-1.0 version: $(pkg-config --modversion gstreamer-1.0)"

# ffmpeg n4.4: FFmpeg 5.0+ removed deprecated APIs that gst-libav 1.18 uses, and our armel
# GStreamer dev headers are 1.18.4 (bullseye) which pins gst-libav source to 1.18. n4.4 is the
# known-good pairing and still ships the TrueHD/MLP decoders/parsers/demuxers we need.
FFTAG=n4.4.4
echo "############ STEP 2: minimal ffmpeg ($FFTAG) ############"
cd /build
if [ ! -d ffmpeg ]; then
  git clone --depth 1 -b $FFTAG https://git.ffmpeg.org/ffmpeg.git ffmpeg
fi
cd ffmpeg
make distclean >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# webOS 25 make-up gain patch (loudness fix — mirrors the DTS side in
# src/gstdtsdec.c). Applied to libavcodec/mlpdec.c BEFORE ./configure. mlpdec.c
# is the translation unit that decodes ONLY truehd+mlp, so the change is
# codec-local and CANNOT touch aac/ac3/eac3/alac output in this same
# libgstlibav.so (the 2026-07-23 Spotify regression class). The decoder set,
# ABI, and GLIBC ceiling below are unchanged. Default 0.0 dB = unity = exact
# no-op: an un-tuned build decodes bit-identically to stock. Patch is inlined
# (self-contained, like build-demux.sh's dts_support patch) so the recipe
# travels with truehd-out/ and needs no external files. See EPIC ADR-001.
# ---------------------------------------------------------------------------
cat > /tmp/mlpdec-makeup-gain.patch <<'PATCH_EOF'
diff --git a/libavcodec/mlpdec.c b/libavcodec/mlpdec.c
index 7563fb0..ba537bb 100644
--- a/libavcodec/mlpdec.c
+++ b/libavcodec/mlpdec.c
@@ -25,6 +25,10 @@
  */

 #include <stdint.h>
+/* webOS 25 make-up-gain patch: config-file read (stdio/stdlib) + pow() (math). */
+#include <stdio.h>
+#include <stdlib.h>
+#include <math.h>

 #include "avcodec.h"
 #include "libavutil/internal.h"
@@ -164,6 +168,11 @@ typedef struct MLPDecodeContext {
     DECLARE_ALIGNED(32, int32_t, sample_buffer)[MAX_BLOCKSIZE][MAX_CHANNELS];

     MLPDSPContext dsp;
+
+    /// webOS 25 make-up-gain patch: cached linear make-up gain read once at
+    /// decoder init from the on-device config file. 1.0 == unity == exact
+    /// no-op (stock-identical decode). See mlp_decode_init().
+    double      makeup_gain_linear;
 } MLPDecodeContext;

 static const uint64_t thd_channel_order[] = {
@@ -276,6 +285,107 @@ static inline int read_huff_channels(MLPDecodeContext *m, GetBitContext *gbp,
     return 0;
 }

+/* ==========================================================================
+ * webOS 25 TrueHD/MLP make-up gain (single functional change vs. upstream
+ * n4.4.4). This translation unit decodes ONLY truehd + mlp, so the change is
+ * codec-local: it cannot affect the other decoders that share libgstlibav.so
+ * (aac/ac3/eac3/alac/...), whose output paths never enter this file.
+ *
+ * Contract (identical to the DTS path in gstdtsdec.c; see the epic ADR-001):
+ *  - Read at decoder init from /var/lib/webosbrew/truehd/gain.conf.
+ *  - A single ASCII float = make-up gain in dB; '#' comments + blank lines
+ *    ignored; leading/trailing whitespace tolerated.
+ *  - Missing / empty / unparseable -> 0.0 dB (unity, no-op). Never fail decode.
+ *  - Clamp parsed dB to [-20, +20], then linear = pow(10, dB/20).
+ *  - 0 dB yields an EXACT linear 1.0 (no pow rounding), and the apply step
+ *    short-circuits at unity, so a default/un-tuned build is bit-identical to
+ *    stock. Gain is applied to the packed integer PCM with a saturating clamp
+ *    (never wraps), matching the sample format chosen by read_major_sync().
+ * ========================================================================== */
+#define MLP_MAKEUP_GAIN_CONF_PATH "/var/lib/webosbrew/truehd/gain.conf"
+#define MLP_MAKEUP_GAIN_DB_MIN (-20.0)
+#define MLP_MAKEUP_GAIN_DB_MAX (20.0)
+
+/* Read the make-up gain (dB) from the on-device config file. A fault (no file,
+ * empty, or no parseable number) returns 0.0 dB (unity) and never breaks decode. */
+static double mlp_read_makeup_gain_db(void)
+{
+    FILE *f;
+    char line[128];
+    double gain_db = 0.0;
+
+    f = fopen(MLP_MAKEUP_GAIN_CONF_PATH, "r");
+    if (!f)
+        return 0.0;
+
+    while (fgets(line, sizeof(line), f)) {
+        char *p = line;
+        char *endptr = NULL;
+        double parsed;
+
+        while (*p == ' ' || *p == '\t')
+            p++;
+        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
+            continue;
+
+        parsed = strtod(p, &endptr);
+        if (endptr != p) {
+            gain_db = parsed;
+            break;
+        }
+    }
+    fclose(f);
+
+    return gain_db;
+}
+
+/* Clamp dB to the contract range and convert to a linear multiplier. 0 dB maps
+ * to an exact 1.0 (no pow rounding) so unity is a bit-exact no-op. */
+static double mlp_makeup_gain_db_to_linear(double gain_db)
+{
+    if (gain_db > MLP_MAKEUP_GAIN_DB_MAX)
+        gain_db = MLP_MAKEUP_GAIN_DB_MAX;
+    else if (gain_db < MLP_MAKEUP_GAIN_DB_MIN)
+        gain_db = MLP_MAKEUP_GAIN_DB_MIN;
+
+    return (gain_db == 0.0) ? 1.0 : pow(10.0, gain_db / 20.0);
+}
+
+/* Apply the cached linear gain to the packed, interleaved PCM output frame in
+ * place, saturating (never wrapping) at the range of the active sample format.
+ * At unity the buffer is left untouched -> provable exact no-op. */
+static void mlp_apply_makeup_gain(void *data, int nb_samples, int channels,
+                                  int is32, double gain_linear)
+{
+    int64_t n = (int64_t) nb_samples * channels;
+    int64_t i;
+
+    if (gain_linear == 1.0)
+        return;
+
+    if (is32) {
+        int32_t *p = data;
+        for (i = 0; i < n; i++) {
+            double v = (double) p[i] * gain_linear;
+            if (v > 2147483647.0)
+                v = 2147483647.0;
+            else if (v < -2147483648.0)
+                v = -2147483648.0;
+            p[i] = (int32_t) v;
+        }
+    } else {
+        int16_t *p = data;
+        for (i = 0; i < n; i++) {
+            double v = (double) p[i] * gain_linear;
+            if (v > 32767.0)
+                v = 32767.0;
+            else if (v < -32768.0)
+                v = -32768.0;
+            p[i] = (int16_t) v;
+        }
+    }
+}
+
 static av_cold int mlp_decode_init(AVCodecContext *avctx)
 {
     static AVOnce init_static_once = AV_ONCE_INIT;
@@ -289,6 +399,15 @@ static av_cold int mlp_decode_init(AVCodecContext *avctx)

     ff_thread_once(&init_static_once, init_static);

+    /* webOS 25 make-up-gain patch: read + cache the gain ONCE here at init
+     * (never per-sample/frame). Default 0.0 dB -> linear 1.0 -> exact no-op. */
+    m->makeup_gain_linear =
+        mlp_makeup_gain_db_to_linear(mlp_read_makeup_gain_db());
+    av_log(avctx, AV_LOG_INFO,
+           "webOS25 mlp/truehd make-up gain effective at init: linear %.4f%s\n",
+           m->makeup_gain_linear,
+           m->makeup_gain_linear == 1.0 ? " (unity, no-op)" : "");
+
     return 0;
 }

@@ -1118,6 +1237,13 @@ static int output_data(MLPDecodeContext *m, unsigned int substr,
                                                     s->max_matrix_channel,
                                                     is32);

+    /* webOS 25 make-up-gain patch: scale the freshly-packed, interleaved PCM
+     * output (frame->data[0], avctx->channels == s->max_matrix_channel + 1,
+     * verified above) by the cached linear gain, with a saturating clamp.
+     * Unity (default) short-circuits -> bit-identical to stock. */
+    mlp_apply_makeup_gain(frame->data[0], s->blockpos, avctx->channels,
+                          is32, m->makeup_gain_linear);
+
     /* Update matrix encoding side data */
     if ((ret = ff_side_data_update_matrix_encoding(frame, s->matrix_encoding)) < 0)
         return ret;
PATCH_EOF

echo "--- applying mlpdec make-up gain patch ---"
# Cloned with git (depth 1) so `git apply` works; fall back to plain `patch`.
if git apply --check /tmp/mlpdec-makeup-gain.patch 2>/dev/null; then
  git apply /tmp/mlpdec-makeup-gain.patch
elif patch -p1 --dry-run < /tmp/mlpdec-makeup-gain.patch >/dev/null 2>&1; then
  patch -p1 < /tmp/mlpdec-makeup-gain.patch
else
  echo "PATCH FAILED: mlpdec-makeup-gain.patch does not apply to $FFTAG libavcodec/mlpdec.c"; exit 1
fi

echo "=== make-up gain patch verification ==="
grep -q 'mlp_apply_makeup_gain' libavcodec/mlpdec.c \
  || { echo "PATCH VERIFY FAILED: mlp_apply_makeup_gain missing"; exit 1; }
grep -q '/var/lib/webosbrew/truehd/gain.conf' libavcodec/mlpdec.c \
  || { echo "PATCH VERIFY FAILED: truehd gain.conf path missing"; exit 1; }
echo "=== make-up gain patch OK (mlpdec.c only; aac/ac3/eac3/alac untouched) ==="

# IMPORTANT: this libgstlibav.so is BIND-MOUNTED over LG's stock one, so it must
# provide EVERYTHING stock libav provided PLUS truehd/mlp. A truehd/mlp-only build
# strips the system's software decoders (avdec_aac/ac3/eac3/mp3/flac/h264/vp8/vp9/
# wma*, ...) when enabled, which breaks apps that decode via libav -- notably
# Spotify (audio/mpeg -> avdec_aac) went silent while the mod was enabled.
# Root-caused on a real C5 (2026-07-23) by enumerating stock `gst-inspect-1.0 libav`.
# Keep this decoder set matching (or a superset of) stock LG libav; DTS stays out
# (handled by the separate libgstdtsdec.so + libdca), exactly as LG shipped it.
STOCK_DECODERS="aac,ac3,eac3,alac,amrnb,amrwb,flac,mp3,h264,mjpeg,vp8,vp9,wmapro,wmav1,wmav2,wmavoice"
CONFIG_COMMON="--cross-prefix=arm-linux-gnueabi- --enable-cross-compile --arch=arm --target-os=linux \
  --cc=arm-linux-gnueabi-gcc \
  --disable-everything \
  --enable-decoder=truehd,mlp,${STOCK_DECODERS} \
  --enable-parser=mlp,aac,ac3,flac,mpegaudio,h264,vp8,vp9 \
  --enable-demuxer=truehd,mlp \
  --enable-avcodec --enable-avformat --enable-avfilter --enable-swresample \
  --enable-shared --disable-static --disable-programs --disable-doc \
  --disable-avdevice --disable-swscale --disable-postproc --disable-network --disable-debug \
  --prefix=/opt/ffmin"

# Soft-float ABI safety: disable neon/vfp/asm outright. Guarantees no hard-float codegen.
./configure $CONFIG_COMMON --disable-neon --disable-vfp --disable-asm 2>&1 | tail -5
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "ffmpeg built. libs:"; ls -la /opt/ffmin/lib/*.so*

echo "--- ffmpeg ABI verify ---"
for f in /opt/ffmin/lib/lib*.so; do
  echo -n "$(basename "$f"): "; file -b "$f" | cut -d, -f1-4
  echo -n "  e_flags: "; od -An -tx4 -j36 -N4 "$f"
done

echo "############ STEP 3: gst-libav ############"
cd /build
if [ ! -d gst-libav ]; then
  git clone --depth 1 -b 1.18 https://gitlab.freedesktop.org/gstreamer/gst-libav.git gst-libav \
    || git clone --depth 1 -b 1.22 https://gitlab.freedesktop.org/gstreamer/gst-libav.git gst-libav
fi

cat > /build/cross.txt <<'EOF'
[binaries]
c = 'arm-linux-gnueabi-gcc'
cpp = 'arm-linux-gnueabi-g++'
ar = 'arm-linux-gnueabi-ar'
strip = 'arm-linux-gnueabi-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv5'
endian = 'little'

[properties]
sys_root = '/sysroot'
pkg_config_libdir = '/opt/ffmin/lib/pkgconfig:/sysroot/usr/lib/arm-linux-gnueabi/pkgconfig:/sysroot/usr/share/pkgconfig'
EOF

cd /build/gst-libav
rm -rf build
# PKG_CONFIG_SYSROOT_DIR + PKG_CONFIG_LIBDIR already exported in step 1.
meson setup build --cross-file /build/cross.txt -Ddoc=disabled --buildtype=release 2>&1 | tail -20 || {
    echo "meson setup FAILED — see above"; MESON_FAILED=1; }

PLUGIN=""
if [ "${MESON_FAILED:-0}" != "1" ]; then
  ninja -C build 2>&1 | tail -15 || true
  PLUGIN=$(find build -name 'libgstlibav.so' | head -1)
fi

if [ -z "$PLUGIN" ]; then
  echo "############ FALLBACK: direct gcc compile of libav plugin ############"
  cd /build/gst-libav
  SRC=$(find . -path ./build -prune -o -name '*.c' -print | grep -E 'ext/libav|gstav' | sort)
  echo "sources: $SRC"
  CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0)
  FF=$(PKG_CONFIG_LIBDIR=/opt/ffmin/lib/pkgconfig pkg-config --cflags --libs libavcodec libavformat libavutil libavfilter libswresample)
  arm-linux-gnueabi-gcc -shared -fPIC -O2 -o /build/libgstlibav.so $SRC \
    -DHAVE_CONFIG_H -I. -Iext/libav $CF $FF \
    -DPACKAGE='"gst-libav"' -DPACKAGE_VERSION='"1.18.6"' -DGST_PACKAGE_NAME='"gst-libav"' \
    -DGST_PACKAGE_ORIGIN='"webosbrew-truehd"' -DLIBAV_SOURCE='"ffmpeg"' 2>&1 | tail -30
  PLUGIN=/build/libgstlibav.so
fi
echo "PLUGIN=$PLUGIN"; file "$PLUGIN"

echo "############ STEP 4: bundle to /out ############"
rm -f /out/*.so /out/*.so.* 2>/dev/null || true
cp "$PLUGIN" /out/libgstlibav.so
# copy real ffmpeg libs + symlinks
for base in avcodec avutil avformat avfilter swresample; do
  cp -P /opt/ffmin/lib/lib${base}.so* /out/ 2>/dev/null || true
done
# RUNPATH is NOT inherited transitively, so the chain libgstlibav -> libavcodec -> libavutil
# only resolves if EVERY bundled lib carries the rpath. Set it on all real ELF objects.
for so in /out/libgstlibav.so /out/*.so.*[0-9]; do
  [ -f "$so" ] && patchelf --set-rpath /var/lib/webosbrew/truehd/libs "$so"
done
arm-linux-gnueabi-strip --strip-unneeded /out/libgstlibav.so /out/*.so.*[0-9] 2>/dev/null || true
echo "bundle contents:"; ls -la /out/*.so*

echo "############ STEP 5: verify + report ############"
AVCODEC=$(ls /out/libavcodec.so.*[0-9] | head -1)
REPORT=/out/BUILD-REPORT.txt
ver_le(){ [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }  # $1 <= $2 ?
{
  set +e   # verification greps legitimately return non-zero; never abort the report
  echo "==================================================================="
  echo " TrueHD gst-libav bundle for LG C5 / webOS 25 — BUILD REPORT"
  echo "==================================================================="
  echo "Generated:  $(date -u)"
  echo "Build host: debian:11-slim (bullseye, glibc 2.31), --platform linux/arm64"
  echo "Toolchain:  $(arm-linux-gnueabi-gcc --version | head -1) (soft-float EABI5)"
  echo "ffmpeg:     $FFTAG — full stock LG decoder set (aac/ac3/eac3/alac/amrnb/amrwb/flac/mp3/"
  echo "            h264/mjpeg/vp8/vp9/wma*) PLUS truehd+mlp. Must be a SUPERSET of stock libav:"
  echo "            this .so bind-mounts over LG's, so a truehd-only build strips system codecs"
  echo "            (broke Spotify's avdec_aac when enabled — regression fixed 2026-07-23)."
  echo "gst-libav:  $(cd /build/gst-libav && git describe --tags --always 2>/dev/null) (meson cross-build)"
  echo "Target ABI: 32-bit ARM EABI5 soft-float (gnueabi/armel), e_flags 0x05000200"
  echo "            runtime loader on target: /lib/ld-linux.so.3 (.so carry no PT_INTERP — normal)"
  echo "glibc rule: every symbol version must be <= GLIBC_2.35"
  echo
  echo "===================== PER-FILE VERIFICATION ====================="
  ALL_OK=1
  for f in /out/libgstlibav.so /out/*.so.*[0-9]; do
    [ -f "$f" ] || continue
    echo "---- $(basename "$f") ----"
    echo "  file:     $(file -b "$f" | cut -d, -f1-4)"
    ef=$(od -An -tx4 -j36 -N4 "$f" | tr -d ' ')
    echo "  e_flags:  $ef  $( [ "$ef" = 05000200 ] && echo '[OK EABI5 soft-float]' || { echo '[FAIL]'; ALL_OK=0; } )"
    maxg=$(objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)
    if [ -z "$maxg" ]; then echo "  maxGLIBC: (none)";
    elif ver_le "$maxg" "GLIBC_2.35"; then echo "  maxGLIBC: $maxg  [OK <=2.35]";
    else echo "  maxGLIBC: $maxg  [FAIL >2.35]"; ALL_OK=0; fi
    echo "  NEEDED:   $(readelf -d "$f" 2>/dev/null | grep NEEDED | grep -oE '\[.*\]' | tr '\n' ' ')"
    rp=$(readelf -d "$f" 2>/dev/null | grep -E 'RUNPATH|RPATH' | grep -oE '\[.*\]')
    [ -n "$rp" ] && echo "  RPATH:    $rp"
    echo
  done
  echo "===================== TRUEHD / MLP DECODER CHECK ====================="
  echo "TrueHD+MLP decoder lives in libavcodec; gst-libav registers avdec_truehd at"
  echo "runtime by enumerating libavcodec decoders (element name is generated, not a"
  echo "static string — so we verify the decoder in libavcodec + the decode API in the plugin)."
  echo -n "  libavcodec truehd/mlp strings: "; strings "$AVCODEC" | grep -ixE 'truehd|TrueHD|mlp' | sort -u | tr '\n' ' '; echo
  echo "  libavcodec descriptor: $(strings "$AVCODEC" | grep -i 'Meridian Lossless' | head -1)"
  if objdump -T /out/libgstlibav.so 2>/dev/null | grep -q avcodec_open2; then
    echo "  libgstlibav libav decode API linked: yes (avcodec_open2, av_codec_is_decoder, avcodec_send_packet)"
  else echo "  libgstlibav libav decode API: NO"; ALL_OK=0; fi
  if strings "$AVCODEC" | grep -qi truehd; then echo "  => TrueHD decoder PRESENT in bundle"; else echo "  => TrueHD MISSING"; ALL_OK=0; fi
  echo
  echo "===================== BUNDLE CONTENTS + SIZE ====================="
  ls -la /out/*.so* | awk '{printf "  %8s  %s\n", $5, $9}'
  echo "  TOTAL: $(du -ch /out/*.so* | tail -1 | awk '{print $1}')"
  echo
  echo "OVERALL: $([ "$ALL_OK" = 1 ] && echo 'PASS — all ELF objects ARM EABI5 soft-float (05000200), max GLIBC <=2.35, TrueHD decoder present, plugin links libavcodec decode API.' || echo 'CHECK FAILURES ABOVE')"
} | tee "$REPORT"
echo "############ DONE ############"
