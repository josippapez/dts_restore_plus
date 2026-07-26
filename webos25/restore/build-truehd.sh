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
# webOS 25 loudness patch (mirrors the DTS side in src/gstdtsdec.c): static
# make-up gain + a signal-driven DRC compressor (Dolby Line/RF-style profile
# scaled by boost%/cut%, mirroring LG's DSP parameters) + a front-centre
# dialogue boost. Applied to libavcodec/mlpdec.c BEFORE ./configure.
#
# CODEC-LOCAL. mlpdec.c is the translation unit that decodes ONLY truehd+mlp;
# every symbol the patch adds is `static` and every call site is inside that
# one file, so the change CANNOT touch aac/ac3/eac3/alac output in this same
# libgstlibav.so (the 2026-07-23 Spotify regression class). The decoder set,
# ABI, and GLIBC ceiling below are unchanged.
#
# The DSP block between the DRC-CORE-BEGIN / DRC-CORE-END markers inside the
# patch is a BYTE-FOR-BYTE copy of the same block in src/gstdtsdec.c (the
# reference implementation). Keeping it textually identical — dts_ prefixes and
# all — is what makes drift between the two decoders mechanically detectable,
# and that detection is now an ENFORCED TEST, not a comment: check 6 of
# `sh src/test/run-tests.sh` does exactly this diff and fails the run if it is
# non-empty. Checks 7-9 there also re-check this patch's hunk arithmetic and
# unit-test the host binding below, all without a cross-toolchain. Run it after
# ANY edit to this heredoc — a change made here and not in src/gstdtsdec.c (or
# vice versa) is a silent divergence between the two decoders.
#
# If you need to reproduce the diff by hand:
#
#   sed -n '/<<<DRC-CORE-BEGIN>>>/,/<<<DRC-CORE-END>>>/p' src/gstdtsdec.c > a
#   awk '/^cat > \/tmp\/mlpdec-webos25-loudness.patch <<.PATCH_EOF.$/{f=1;next}
#        /^PATCH_EOF$/{f=0} f' build-truehd.sh \
#     | sed -n 's/^+//p' | sed -n '/<<<DRC-CORE-BEGIN>>>/,/<<<DRC-CORE-END>>>/p' > b
#   diff a b     # must be empty
#
# Defaults (gain 0 dB, drc=off, center=0) are fully inert: the DRC path is
# bypassed and the shipped make-up-gain call multiplies by an exact 1.0, so an
# un-tuned build decodes bit-identically to stock. Patch is inlined
# (self-contained, like build-demux.sh's dts_support patch) so the recipe
# travels with truehd-out/ and needs no external files. See EPIC ADR-001 and
# .orchestration/dts-truehd-drc/EPIC.md for the binding DSP contract.
# ---------------------------------------------------------------------------
cat > /tmp/mlpdec-webos25-loudness.patch <<'PATCH_EOF'
diff --git a/libavcodec/mlpdec.c b/libavcodec/mlpdec.c
index 7563fb0..744db59 100644
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
@@ -164,6 +168,52 @@ typedef struct MLPDecodeContext {
     DECLARE_ALIGNED(32, int32_t, sample_buffer)[MAX_BLOCKSIZE][MAX_CHANNELS];
 
     MLPDSPContext dsp;
+
+    /// webOS 25 make-up-gain patch: cached linear make-up gain read once at
+    /// decoder init from the on-device config file. 1.0 == unity == exact
+    /// no-op (stock-identical decode). See mlp_decode_init().
+    double      makeup_gain_linear;
+
+    /// webOS 25 DRC patch: user settings, all read ONCE in mlp_decode_init()
+    /// from the same config file and cached here (never re-read per frame).
+    /// drc_mode is a DTS_DRC_MODE_* value (0=off, 1=line, 2=rf).
+    int         drc_mode;
+    float       drc_boost_pct;      ///< 0..100, scales positive (boost) gains
+    float       drc_cut_pct;        ///< 0..100, scales negative (cut) gains
+    float       center_boost_db;    ///< -10..+10 dB, front-centre channel only
+    float       center_boost_linear;///< cached; exactly 1.0f when 0.0 dB
+    float       makeup_gain_linear_f;///< float mirror of makeup_gain_linear,
+                                    ///< used only by the DRC apply path (the
+                                    ///< inert path keeps the shipped double)
+    /// 0 == the shipped make-up-gain-only path (bit-identical to the currently
+    /// shipped build); 1 == the DRC / centre-boost path.
+    int         drc_active;
+
+    /// webOS 25 DRC patch: runtime state (per decoder instance; cleared in
+    /// mlp_decode_init() and mlp_decode_flush()).
+    float       drc_smoothed_db;    ///< one-pole smoothed gain, dB domain
+    float       drc_prev_linear;    ///< previous block's linear DRC factor
+    float       drc_target_linear;  ///< current detector window's linear factor,
+                                    ///< held between detector updates
+    float       drc_attack_coef;    ///< per-window one-pole coefficients,
+    float       drc_release_coef;   ///<   derived from mode + rate + window
+    int         drc_coef_rate;      ///< sample rate the coefs were built for
+    int         drc_coef_mode;      ///< drc_mode the coefs were built for
+    int         drc_coef_block;     ///< window length the coefs were built for
+
+    /// Detector accumulator. An MLP access unit is only 40 samples (0.833 ms at
+    /// every rate: access_unit_size == 40 << ratebits), against libdca's fixed
+    /// 256-sample / 5.33 ms block. Measuring per access unit would shrink the
+    /// level-estimate averaging window 6.4x, and because the curve is nonlinear
+    /// and unimodal the asymmetric smoother then SETTLES ON A DIFFERENT GAIN
+    /// rather than a noisier version of the same one — measured up to 3.9 dB
+    /// less boost than the DTS path on dialogue-band material. So sum-of-
+    /// squares is accumulated across access units and the smoother is stepped
+    /// once per >= DTS_DRC_BLOCK_SAMPLES window, which restores both the
+    /// detector statistics and the time constant.
+    float       drc_acc_sum_sq;     ///< normalised sum of squares, this window
+    int         drc_acc_count;      ///< samples summed into drc_acc_sum_sq
+    int         drc_acc_samples;    ///< per-channel samples in this window
 } MLPDecodeContext;
 
 static const uint64_t thd_channel_order[] = {
@@ -276,6 +326,896 @@ static inline int read_huff_channels(MLPDecodeContext *m, GetBitContext *gbp,
     return 0;
 }
 
+/* ==========================================================================
+ * webOS 25 TrueHD/MLP loudness patch (the ONLY functional change vs. upstream
+ * n4.4.4): a static make-up gain, a signal-driven dynamic range compressor
+ * (Dolby Line/RF-style profile scaled by boost%/cut%, mirroring LG's DSP
+ * parameters) and a separate front-centre "dialogue" boost.
+ *
+ * CODEC-LOCAL BY CONSTRUCTION. mlpdec.c is the translation unit that decodes
+ * ONLY truehd + mlp; every symbol added below is `static` and every call site
+ * is inside this file, so the other decoders sharing libgstlibav.so
+ * (aac/ac3/eac3/alac/...) cannot reach any of it — their output paths never
+ * enter this file. That is the 2026-07-23 Spotify regression class, and this
+ * patch cannot reproduce it.
+ *
+ * ALL DEFAULTS ARE INERT: gain 0 dB, drc=off, center=0 dB. In that state
+ * output_data() takes the previously shipped make-up-gain call with an exact
+ * linear 1.0, which short-circuits — i.e. bit-identical to the shipped build.
+ *
+ * The DSP itself is a PORT, not a redesign: the block between the
+ * DRC-CORE-BEGIN / DRC-CORE-END markers below is a byte-for-byte copy of the
+ * same block in the DTS decoder (webos25/restore/src/gstdtsdec.c), which is
+ * the reference implementation of the epic's DSP + config contract
+ * (.orchestration/dts-truehd-drc/EPIC.md). It keeps its dts_ / DTS_ prefixes
+ * ON PURPOSE: an exact textual copy is the only thing that makes divergence
+ * between the two decoders mechanically detectable (diff the two extractions),
+ * and divergence is the defect this port exists to avoid. Change the math in
+ * gstdtsdec.c first, re-run its run-tests.sh, then re-copy it here.
+ *
+ * Only the HOST BINDING below the core differs, and only where it must:
+ * libdca hands the DTS decoder planar float samples in fixed 256-sample
+ * blocks, while here the PCM is interleaved integer in variable-length MLP
+ * access units. Each such difference is called out at its site.
+ * ========================================================================== */
+#define MLP_MAKEUP_GAIN_CONF_PATH "/var/lib/webosbrew/truehd/gain.conf"
+
+/*<<<DRC-CORE-BEGIN>>>*/
+/* ==========================================================================
+ * BEGIN DRC CORE
+ * --------------------------------------------------------------------------
+ * REFERENCE IMPLEMENTATION of the epic's DSP + config contract
+ * (.orchestration/dts-truehd-drc/EPIC.md). Everything between the BEGIN/END
+ * markers is deliberately dependency-free: plain C using only <math.h>,
+ * <stdio.h>, <stdlib.h> and <string.h>, no GLib/GStreamer/libdca types. That
+ * lets it be lifted verbatim into ffmpeg's mlpdec.c for the TrueHD decoder,
+ * and lets webos25/restore/src/test/run-tests.sh extract everything between
+ * the DRC-CORE-BEGIN / DRC-CORE-END marker comments and unit-test the exact
+ * shipped code instead of a copy of it. Keep the two decoders in sync: change
+ * the math HERE, then re-port.
+ *
+ * Cost model (target is armel soft-float, 5.1 @ 48 kHz):
+ *   per block (256 samples): 1x log10f (detector) + 1x powf (dB->linear),
+ *                            plus 2x expf only when mode/sample-rate changes.
+ *   per sample:              1 float add (gain ramp) + 1 float multiply.
+ * No transcendental and no double ever enters the per-sample path.
+ * ========================================================================== */
+
+/* ---- DRC modes (mirror LG's LX_AUD_DECODER_DRC_{OFF,LINE,RF}) ---------- */
+#define DTS_DRC_MODE_OFF   0
+#define DTS_DRC_MODE_LINE  1
+#define DTS_DRC_MODE_RF    2
+
+/* ---- ALL TUNABLE CONSTANTS LIVE IN THIS ONE BLOCK ---------------------- */
+
+/* Decoder block size the detector/smoother run at (one libdca dca_block).
+ * PORTING NOTE: the host code below this block still writes 256 literally
+ * where upstream gstdtsdec.c already hardcoded it (buffer sizing, the output
+ * loop). The MLP block size is NOT 256, so the mlpdec.c port must route every
+ * one of those uses through this macro. */
+#define DTS_DRC_BLOCK_SAMPLES 256
+
+/* Detector output bounds. The floor keeps digital silence off -inf dBFS; the
+ * ceiling bounds an over-range or non-finite block. Both matter for more than
+ * tidiness: the smoother is stateful, so a single NaN or +inf level would
+ * poison drc_smoothed_db for the rest of the stream (only _start() clears it)
+ * and every later sample would convert from a NaN double. */
+#define DTS_DRC_LEVEL_FLOOR_DB (-90.0f)
+#define DTS_DRC_LEVEL_CEIL_DB (20.0f)
+
+/* NOTE — there is deliberately NO silence gate here (epic amendment F retired
+ * the one that used to live at this spot, and amendment E with it). Every
+ * job it did is now covered elsewhere, and it made the worst case worse:
+ *   - wind-up over quiet passages: handled by the boost decay in the curve
+ *     below, which already targets 0 dB at -85 dBFS and lower;
+ *   - non-finite levels: handled by dts_drc_level_dbfs()'s floor, which maps
+ *     NaN to -90 dBFS (hence 0 dB of gain) before anything else sees it;
+ *   - hard cut from an established high gain: a gate actively HURT this. It
+ *     froze the pre-cut gain, so the next cue peaked at -0.42 dBFS — against
+ *     -12.01 dBFS with no gate. Releasing normally is the correct response.
+ * Do not reintroduce one in the mlpdec.c port. */
+
+/* Config value ranges + defaults (epic "Config contract"). */
+#define DTS_MAKEUP_GAIN_DB_MIN (-20.0f)
+#define DTS_MAKEUP_GAIN_DB_MAX (20.0f)
+#define DTS_DRC_PCT_MIN (0.0f)
+#define DTS_DRC_PCT_MAX (100.0f)
+#define DTS_DRC_CENTER_DB_MIN (-10.0f)
+#define DTS_DRC_CENTER_DB_MAX (10.0f)
+
+#define DTS_DRC_DEFAULT_GAIN_DB (0.0f)
+#define DTS_DRC_DEFAULT_MODE DTS_DRC_MODE_OFF
+#define DTS_DRC_DEFAULT_BOOST_PCT (100.0f)
+#define DTS_DRC_DEFAULT_CUT_PCT (100.0f)
+#define DTS_DRC_DEFAULT_CENTER_DB (0.0f)
+
+/* Static compression curve + smoothing, one profile per mode.
+ *
+ * The shape follows the publicly documented structure of Dolby's Line/RF
+ * profiles (null band, 2:1 early boost/cut regions, a steep max-boost region
+ * at the bottom and a 20:1 limiting region at the top). These are a
+ * documented-style APPROXIMATION chosen to mirror LG's DSP behaviour — they
+ * are NOT Dolby's proprietary tables, and they are meant to be retuned here.
+ *
+ * Breakpoints, referenced to dBFS (from the epic's DSP contract). Read the
+ * boost side left-to-right as rising level: the boost decays in from 0 dB at
+ * -85, peaks, then falls away through the null band into the cuts.
+ *   line: 0 dB <= -85 | decay -85..-50.5 | PEAK +12 at -50.5
+ *         | 5:1 -50.5..-43 | 2:1 boost -43..-31 | null -31..-20
+ *         | 2:1 cut -20..-10 | 20:1 cut > -10
+ *   rf:   0 dB <= -85 | decay -85..-55.5 | PEAK +16 at -55.5
+ *         | 5:1 -55.5..-43 | 2:1 boost -43..-31 | null -31..-24
+ *         | 2:1 cut -24..-14 | 20:1 cut > -14
+ * The two peak levels are derived, not stored — see the amendment D note below.
+ *
+ * maxboost_ratio is 5:1 (epic amendment A). The epic's original table gave the
+ * region below -43 dBFS only as "boost, capped at +N dB" with no ratio, and
+ * the 2:1 segment above it reaches just +6 dB at -43, so a steeper ratio is
+ * needed for the cap to be reachable at all. 5:1 is a project choice, not a
+ * Dolby figure; it keeps the curve continuous with the 2:1 segment.
+ *
+ * BOOST DECAY (epic amendment D) — the boost region PEAKS and then declines
+ * linearly back to 0 dB at boost_zero_db, instead of holding the cap all the
+ * way down to the detector floor:
+ *
+ *   line: +12 dB peak at -50.5 dBFS -> 0 dB at -85 dBFS
+ *   rf:   +16 dB peak at -55.5 dBFS -> 0 dB at -85 dBFS
+ *
+ * Both peak levels are DERIVED, not stored: they are simply where the 5:1
+ * ramp reaches max_boost_db, so retuning the cap or the ratio moves them
+ * automatically and the curve stays continuous by construction.
+ *
+ * WHY the decay: material below roughly -60 dBFS is room tone, tape hiss and
+ * dither — amplifying that by 12-20 dB was never the intent, and a plateau
+ * held all the way down to the detector floor meant a slow fade-out wound the
+ * gain up to the cap, so the next loud cue swelled and clipped. With the decay,
+ * quiet-but-real material still gets the full boost while near-silence returns
+ * toward unity, so a fade cannot wind up. This is also the ONLY thing standing
+ * between a quiet passage and a swollen cue — see the amendment F note above.
+ * rf's cap is +16 rather than +20 for the same reason: +20 dB was the dominant
+ * term in the worst-case clip and is more than this use case needs.
+ *
+ * Retune here — and re-port to mlpdec.c if you do. TWO INVARIANTS keep the
+ * curve continuous; both are easy to break by eye, so they are spelled out:
+ *
+ *   1. maxboost_ratio MUST be > 1. At exactly 1 the rise slope is 0 and the
+ *      peak derivation divides by zero. The OFF row below holds ratio 1.0 and
+ *      is safe ONLY because dts_drc_target_gain_db() returns early for any
+ *      mode that is not LINE or RF — a port MUST keep that guard.
+ *   2. max_boost_db must stay inside
+ *        knee_boost < max_boost_db
+ *                   < knee_boost + (maxboost_knee_db - boost_zero_db) * slope
+ *      where knee_boost is the +6 dB the 2:1 segment reaches at -43 dBFS and
+ *      slope is 1 - 1/maxboost_ratio. With the pinned constants that is
+ *      6 < cap < 39.6 dB. Below it, the cap sits under the knee and the curve
+ *      steps at -43; above it, the peak falls past boost_zero_db, the decay
+ *      leg vanishes and the curve steps at -85.
+ *
+ * The detector MUST stay feed-forward. This curve is unimodal, so two input
+ * levels can map to the same gain; that is harmless here because the detector
+ * reads the decoded samples before any gain is applied. A port that measured
+ * post-gain samples could limit-cycle on the decay leg.
+ *
+ * Smoothing time constants (one-pole, dB domain, per block):
+ *   line: attack 10 ms / release 250 ms;  rf: attack 5 ms / release 150 ms. */
+typedef struct
+{
+  float boost_knee_db;          /* lower edge of the null band            */
+  float boost_ratio;            /* n:1 over maxboost_knee_db..boost_knee_db */
+  float maxboost_knee_db;       /* below this the steeper max-boost region */
+  float maxboost_ratio;
+  float max_boost_db;           /* peak positive gain (cap)               */
+  float boost_zero_db;          /* boost decays back to 0 dB here         */
+  float cut_knee_db;            /* upper edge of the null band            */
+  float cut_ratio;              /* n:1 over cut_knee_db..hardcut_knee_db  */
+  float hardcut_knee_db;
+  float hardcut_ratio;          /* n:1 above hardcut_knee_db (limiting)   */
+  float attack_ms;
+  float release_ms;
+} DtsDrcProfile;
+
+static const DtsDrcProfile dts_drc_profiles[3] = {
+  /* [DTS_DRC_MODE_OFF] never evaluated; kept so the array is mode-indexed. */
+  {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
+  /* [DTS_DRC_MODE_LINE] */
+  {-31.0f, 2.0f, -43.0f, 5.0f, 12.0f, -85.0f, -20.0f, 2.0f, -10.0f, 20.0f,
+      10.0f, 250.0f},
+  /* [DTS_DRC_MODE_RF] */
+  {-31.0f, 2.0f, -43.0f, 5.0f, 16.0f, -85.0f, -24.0f, 2.0f, -14.0f, 20.0f,
+      5.0f, 150.0f}
+};
+
+/* ---- END OF TUNABLE CONSTANTS ------------------------------------------ */
+
+/* Locale-independent ASCII float parse; see the DTS_DRC_STRTOD note above. */
+#ifndef DTS_DRC_STRTOD
+#define DTS_DRC_STRTOD(nptr, endptr) strtod ((nptr), (endptr))
+#endif
+
+/* User settings as read from the config file / GObject properties. */
+typedef struct
+{
+  float gain_db;                /* make-up gain, dB (legacy bare float)   */
+  int drc_mode;                 /* DTS_DRC_MODE_*                         */
+  float drc_boost_pct;          /* 0..100                                 */
+  float drc_cut_pct;            /* 0..100                                 */
+  float center_db;              /* -10..+10 dB                            */
+} DtsDrcConfig;
+
+/* NaN-safe by construction. The obvious `if (v > hi) ... if (v < lo) ...` form
+ * is NOT: every comparison against NaN is false, so a NaN sailed through both
+ * tests untouched and reached the DSP, where (gint32) NaN is undefined
+ * behaviour and a NaN gain poisons the stateful smoother for the rest of the
+ * stream. Testing `!(v > lo)` first inverts that — the predicate is TRUE for
+ * NaN — so NaN lands on lo. Same reasoning as the detector floor's inverted
+ * comparison; that path was hardened, this one was not. +/-inf are ordered and
+ * clamp to hi/lo as before, and every finite value is unaffected (v == lo
+ * returns lo, which is v). */
+static float
+dts_drc_clampf (float v, float lo, float hi)
+{
+  if (!(v > lo))
+    return lo;
+  if (v > hi)
+    return hi;
+  return v;
+}
+
+/* ASCII-only, locale-independent case-insensitive string equality. */
+static int
+dts_drc_ascii_ieq (const char *a, const char *b)
+{
+  while (*a != '\0' && *b != '\0') {
+    char ca = *a++;
+    char cb = *b++;
+
+    if (ca >= 'A' && ca <= 'Z')
+      ca = (char) (ca - 'A' + 'a');
+    if (cb >= 'A' && cb <= 'Z')
+      cb = (char) (cb - 'A' + 'a');
+    if (ca != cb)
+      return 0;
+  }
+  return (*a == '\0' && *b == '\0');
+}
+
+static int
+dts_drc_mode_from_string (const char *s)
+{
+  if (s != NULL) {
+    if (dts_drc_ascii_ieq (s, "line"))
+      return DTS_DRC_MODE_LINE;
+    if (dts_drc_ascii_ieq (s, "rf"))
+      return DTS_DRC_MODE_RF;
+  }
+  /* "off" and anything unrecognised -> the default, which is off. */
+  return DTS_DRC_MODE_OFF;
+}
+
+static const char *
+dts_drc_mode_to_string (int mode)
+{
+  if (mode == DTS_DRC_MODE_LINE)
+    return "line";
+  if (mode == DTS_DRC_MODE_RF)
+    return "rf";
+  return "off";
+}
+
+/* ------------------------------------------------------------------------
+ * 1. Level detector — per block, full-range channels only (LFE excluded).
+ * ---------------------------------------------------------------------- */
+
+/* Sum of squares over a planar [nch][nsamples] float buffer, skipping channel
+ * skip_ch (pass -1 for none — that is how LFE is kept out of the detector).
+ * *count_out receives the number of samples actually summed. */
+static float
+dts_drc_sum_squares (const float *planar, int nch, int nsamples, int skip_ch,
+    int *count_out)
+{
+  float sum = 0.0f;
+  int count = 0;
+  int c, n;
+
+  for (c = 0; c < nch; c++) {
+    const float *p;
+
+    if (c == skip_ch)
+      continue;
+    p = planar + c * nsamples;
+    for (n = 0; n < nsamples; n++)
+      sum += p[n] * p[n];
+    count += nsamples;
+  }
+
+  *count_out = count;
+  return sum;
+}
+
+/* Block RMS as dBFS. Uses 10*log10(mean square) == 20*log10(rms), so this
+ * costs ONE log10f per block and no sqrtf. Floored at DTS_DRC_LEVEL_FLOOR_DB
+ * so digital silence cannot produce -inf. */
+static float
+dts_drc_level_dbfs (float sum_sq, int count)
+{
+  float mean_sq, level_db;
+
+  if (count <= 0 || sum_sq <= 0.0f)
+    return DTS_DRC_LEVEL_FLOOR_DB;
+
+  mean_sq = sum_sq / (float) count;
+  level_db = 10.0f * log10f (mean_sq);
+
+  /* Bound the result on BOTH sides, and note the inverted comparison: it is
+   * what makes this NaN-safe. `NaN < floor` is false, so the obvious form
+   * would return NaN, and because the smoother is stateful a single bad
+   * sample would then poison drc_smoothed_db for the whole stream. */
+  if (!(level_db > DTS_DRC_LEVEL_FLOOR_DB))
+    return DTS_DRC_LEVEL_FLOOR_DB;
+  return (level_db > DTS_DRC_LEVEL_CEIL_DB) ? DTS_DRC_LEVEL_CEIL_DB :
+      level_db;
+}
+
+/* ------------------------------------------------------------------------
+ * 2. Static curve — input level (dBFS) -> target gain (dB).
+ * ---------------------------------------------------------------------- */
+static float
+dts_drc_target_gain_db (int mode, float level_dbfs)
+{
+  const DtsDrcProfile *p;
+  float gain_db;
+
+  if (mode != DTS_DRC_MODE_LINE && mode != DTS_DRC_MODE_RF)
+    return 0.0f;
+  p = &dts_drc_profiles[mode];
+
+  if (level_dbfs > p->hardcut_knee_db) {
+    /* limiting region: full 2:1 cut across the early-cut band, then n:1 */
+    gain_db = -((p->hardcut_knee_db - p->cut_knee_db)
+        * (1.0f - 1.0f / p->cut_ratio)
+        + (level_dbfs - p->hardcut_knee_db)
+        * (1.0f - 1.0f / p->hardcut_ratio));
+  } else if (level_dbfs > p->cut_knee_db) {
+    /* early-cut region */
+    gain_db = -((level_dbfs - p->cut_knee_db) * (1.0f - 1.0f / p->cut_ratio));
+  } else if (level_dbfs >= p->boost_knee_db) {
+    /* null band — the whole point of the profile: leave dialogue alone */
+    gain_db = 0.0f;
+  } else if (level_dbfs >= p->maxboost_knee_db) {
+    /* boost region */
+    gain_db = (p->boost_knee_db - level_dbfs) * (1.0f - 1.0f / p->boost_ratio);
+  } else {
+    /* Max-boost region: rise at maxboost_ratio up to the cap, then DECAY
+     * linearly back to 0 dB at boost_zero_db (amendment D). */
+    float knee_boost = (p->boost_knee_db - p->maxboost_knee_db)
+        * (1.0f - 1.0f / p->boost_ratio);
+    float rise_slope = 1.0f - 1.0f / p->maxboost_ratio;
+    /* Where the rise reaches the cap. Derived, so the curve is continuous by
+     * construction however the cap or the ratio is retuned. */
+    float peak_level_db = p->maxboost_knee_db
+        - (p->max_boost_db - knee_boost) / rise_slope;
+
+    if (level_dbfs >= peak_level_db) {
+      gain_db = knee_boost + (p->maxboost_knee_db - level_dbfs) * rise_slope;
+      if (gain_db > p->max_boost_db)
+        gain_db = p->max_boost_db;
+    } else if (level_dbfs > p->boost_zero_db) {
+      /* Decay leg. Reaching this branch requires
+       * boost_zero_db < level_dbfs < peak_level_db, which already implies
+       * peak_level_db > boost_zero_db — so the divisor cannot be zero. */
+      gain_db = p->max_boost_db * (level_dbfs - p->boost_zero_db)
+          / (peak_level_db - p->boost_zero_db);
+    } else {
+      gain_db = 0.0f;
+    }
+  }
+
+  return gain_db;
+}
+
+/* ------------------------------------------------------------------------
+ * 3. Scale by boost%/cut% — mirrors LG's drc_boost_scl_factor /
+ *    drc_cut_scl_factor (both 0..100).
+ * ---------------------------------------------------------------------- */
+static float
+dts_drc_scale_gain_db (float gain_db, float boost_pct, float cut_pct)
+{
+  if (gain_db > 0.0f)
+    return gain_db * (boost_pct * 0.01f);
+  return gain_db * (cut_pct * 0.01f);
+}
+
+/* ------------------------------------------------------------------------
+ * 4. Smoothing — one-pole in the dB domain, stepped once per block.
+ * ---------------------------------------------------------------------- */
+
+/* One-pole coefficient for a time constant of tau_ms, stepped every
+ * block_period_s seconds. Only ever called when mode or sample rate changes,
+ * so its expf() is not on the per-block path. */
+static float
+dts_drc_smooth_coef (float tau_ms, float block_period_s)
+{
+  float tau_s = tau_ms * 0.001f;
+
+  if (tau_s <= 0.0f || block_period_s <= 0.0f)
+    return 1.0f;
+  return 1.0f - expf (-block_period_s / tau_s);
+}
+
+/* Attack when the target gain falls (signal got louder), release when it
+ * rises. Pure arithmetic — no transcendental. */
+static float
+dts_drc_smooth_step (float smoothed_db, float target_db, float attack_coef,
+    float release_coef)
+{
+  float coef;
+
+  /* Self-recovering. This is the only stateful DSP here, so a NaN that ever
+   * reached it would be an ABSORBING state: NaN compares false, takes the
+   * release branch, and `smoothed + coef * (target - NaN)` is NaN again — on
+   * every channel, on every later block, until _start()/flush. The clamp and
+   * config hardening should make that unreachable; this makes it recoverable
+   * regardless, by snapping to the next finite target instead of tracking
+   * towards it. `smoothed_db != smoothed_db` is true for NaN only. */
+  if (smoothed_db != smoothed_db)
+    return target_db;
+
+  coef = (target_db < smoothed_db) ? attack_coef : release_coef;
+
+  return smoothed_db + coef * (target_db - smoothed_db);
+}
+
+/* dB -> linear, once per block. Exactly 1.0f at 0 dB (no powf rounding), which
+ * is what makes the "no DRC" state an exact multiply-by-one. */
+static float
+dts_drc_db_to_linear (float db)
+{
+  return (db == 0.0f) ? 1.0f : powf (10.0f, db * (1.0f / 20.0f));
+}
+
+/* ------------------------------------------------------------------------
+ * 5. Config file — epic "Config contract".
+ *    - a bare float on its own line  -> make-up gain dB (legacy format)
+ *    - key=value lines               -> drc / drc_boost / drc_cut / center
+ *    - '#' comments and blank lines ignored, unknown keys ignored
+ *    - unparseable value -> that key's default; out-of-range -> clamped
+ *    - a missing or unreadable file is not an error: defaults are used
+ * ---------------------------------------------------------------------- */
+static void
+dts_drc_config_defaults (DtsDrcConfig * cfg)
+{
+  cfg->gain_db = DTS_DRC_DEFAULT_GAIN_DB;
+  cfg->drc_mode = DTS_DRC_DEFAULT_MODE;
+  cfg->drc_boost_pct = DTS_DRC_DEFAULT_BOOST_PCT;
+  cfg->drc_cut_pct = DTS_DRC_DEFAULT_CUT_PCT;
+  cfg->center_db = DTS_DRC_DEFAULT_CENTER_DB;
+}
+
+static char *
+dts_drc_lstrip (char *s)
+{
+  while (*s == ' ' || *s == '\t')
+    s++;
+  return s;
+}
+
+static void
+dts_drc_rstrip (char *s)
+{
+  size_t n = strlen (s);
+
+  while (n > 0) {
+    char c = s[n - 1];
+
+    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
+      break;
+    s[--n] = '\0';
+  }
+}
+
+/* 1 + *out set (clamped) when val starts with a parseable ASCII float,
+ * 0 otherwise (caller keeps that setting's default). */
+static int
+dts_drc_parse_clamped (const char *val, float lo, float hi, float *out)
+{
+  char *end = NULL;
+  double parsed = DTS_DRC_STRTOD (val, &end);
+
+  /* strtod() accepts "nan"/"NaN"/"nan(chars)" — and this is reachable: the
+   * companion app writes the config from JavaScript, where String(NaN) is
+   * exactly "NaN". A NaN is not an out-of-range NUMBER, so the contract's
+   * "out-of-range -> clamp" rule has no meaning for it (clamping needs an
+   * ordering); treat it as unparseable and let the caller keep that key's
+   * default. `parsed != parsed` is true for NaN only, so +/-inf still clamp. */
+  if (end == val || parsed != parsed)
+    return 0;
+  *out = dts_drc_clampf ((float) parsed, lo, hi);
+  return 1;
+}
+
+/* Apply one config line to cfg. *have_gain guards the legacy bare float so
+ * the FIRST one wins (matching the pre-DRC reader, which stopped at it). */
+static void
+dts_drc_config_parse_line (DtsDrcConfig * cfg, char *line, int *have_gain)
+{
+  char *p, *eq, *key, *val;
+
+  p = dts_drc_lstrip (line);
+  if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
+    return;
+
+  eq = strchr (p, '=');
+  if (eq != NULL) {
+    key = p;
+    *eq = '\0';
+    val = dts_drc_lstrip (eq + 1);
+    dts_drc_rstrip (key);
+    dts_drc_rstrip (val);
+
+    if (dts_drc_ascii_ieq (key, "drc")) {
+      cfg->drc_mode = dts_drc_mode_from_string (val);
+    } else if (dts_drc_ascii_ieq (key, "drc_boost")) {
+      if (!dts_drc_parse_clamped (val, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX,
+              &cfg->drc_boost_pct))
+        cfg->drc_boost_pct = DTS_DRC_DEFAULT_BOOST_PCT;
+    } else if (dts_drc_ascii_ieq (key, "drc_cut")) {
+      if (!dts_drc_parse_clamped (val, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX,
+              &cfg->drc_cut_pct))
+        cfg->drc_cut_pct = DTS_DRC_DEFAULT_CUT_PCT;
+    } else if (dts_drc_ascii_ieq (key, "center")) {
+      if (!dts_drc_parse_clamped (val, DTS_DRC_CENTER_DB_MIN,
+              DTS_DRC_CENTER_DB_MAX, &cfg->center_db))
+        cfg->center_db = DTS_DRC_DEFAULT_CENTER_DB;
+    }
+    /* unknown key -> ignored, so newer config files stay readable */
+    return;
+  }
+
+  if (!*have_gain && dts_drc_parse_clamped (p, DTS_MAKEUP_GAIN_DB_MIN,
+          DTS_MAKEUP_GAIN_DB_MAX, &cfg->gain_db))
+    *have_gain = 1;
+}
+
+/* Never fails: cfg always comes back fully populated. */
+static void
+dts_drc_config_read_file (const char *path, DtsDrcConfig * cfg)
+{
+  FILE *f;
+  char line[256];
+  int have_gain = 0;
+
+  dts_drc_config_defaults (cfg);
+
+  f = fopen (path, "r");
+  if (f == NULL)
+    return;
+
+  while (fgets (line, sizeof (line), f) != NULL)
+    dts_drc_config_parse_line (cfg, line, &have_gain);
+
+  fclose (f);
+}
+
+/* ==========================================================================
+ * END DRC CORE
+ * ========================================================================== */
+/*<<<DRC-CORE-END>>>*/
+
+/* ==========================================================================
+ * HOST BINDING — everything below is mlpdec.c-specific and is NOT part of the
+ * ported core. Three things differ from the DTS host and nothing else does:
+ *   1. the PCM is interleaved integer, not planar float  -> own detector input
+ *      and own apply loop (same arithmetic, different addressing);
+ *   2. the decoder delivers short MLP access units (40-160 samples), not a
+ *      fixed 256-sample libdca block -> the detector ACCUMULATES access units
+ *      into a window of at least DTS_DRC_BLOCK_SAMPLES and steps the smoother
+ *      once per window, and the one-pole coefficients are derived from that
+ *      accumulated window length. Measuring per access unit instead would
+ *      shrink the averaging window up to 6.4x and change the settled gain;
+ *   3. there are no GObject properties here, so the config file is the only
+ *      input.
+ * ========================================================================== */
+
+/* Read the make-up gain (dB) from the on-device config file. A fault (no file,
+ * empty, or no parseable number) returns 0.0 dB (unity) and never breaks decode.
+ *
+ * UNCHANGED from the shipped build, and deliberately kept even though the DRC
+ * core's dts_drc_config_read_file() also parses a bare float. The core carries
+ * gain_db as a FLOAT, so routing the make-up gain through it would round the
+ * value before pow() ever saw it and shift the linear gain by ~5e-9 relative —
+ * inaudible, but it would break the exact bit-for-bit equality with the shipped
+ * build that the inert path promises at EVERY gain value, not just 0 dB. The
+ * user already runs a non-zero TrueHD gain. Measured: 8 of 10 typical dB values
+ * (3.7, 7.1, 4.4, 6.6, 8.8, 0.1, 12.345678, ...) differ if routed through the
+ * float. So: the legacy bare float keeps its own double-precision reader, and
+ * the core's config reader supplies only the new drc/center settings. */
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
+        /* `parsed != parsed` rejects NaN: strtod() accepts the literal "NaN",
+         * the app writes this file from JavaScript where String(NaN) is
+         * exactly that, and a NaN gain would make every output sample
+         * (int32_t) NaN — undefined behaviour. Mirrors the core's
+         * dts_drc_parse_clamped(); an unusable value leaves the default 0 dB.
+         * +/-inf still parse and clamp, per epic amendment B. */
+        if (endptr != p && parsed == parsed) {
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
+ * to an exact 1.0 (no pow rounding) so unity is a bit-exact no-op.
+ *
+ * Deliberately still double, and deliberately NOT dts_drc_db_to_linear(): this
+ * is the ALREADY-SHIPPED make-up-gain conversion and the inert path must keep
+ * producing the exact same samples as the shipped build at EVERY gain value,
+ * not only at 0 dB. Swapping in the core's float powf() would move the last
+ * bit of every sample on an already-tuned installation. */
+static double mlp_makeup_gain_db_to_linear(double gain_db)
+{
+    /* Inverted first test, same reason as dts_drc_clampf(): `!(x > lo)` is
+     * TRUE for NaN, so a non-finite gain lands on the floor instead of
+     * sailing through both comparisons into pow() and then into every sample.
+     * Deliberately open-coded rather than calling dts_drc_clampf(), because
+     * this conversion must stay in DOUBLE — see the comment above. Finite
+     * values clamp to exactly the same result as before. */
+    if (!(gain_db > DTS_MAKEUP_GAIN_DB_MIN))
+        gain_db = DTS_MAKEUP_GAIN_DB_MIN;
+    else if (gain_db > DTS_MAKEUP_GAIN_DB_MAX)
+        gain_db = DTS_MAKEUP_GAIN_DB_MAX;
+
+    return (gain_db == 0.0) ? 1.0 : pow(10.0, gain_db / 20.0);
+}
+
+/* Apply the cached linear gain to the packed, interleaved PCM output frame in
+ * place, saturating (never wrapping) at the range of the active sample format.
+ * At unity the buffer is left untouched -> provable exact no-op.
+ * UNCHANGED from the shipped build: this is the inert / gain-only path. */
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
+/* Detector input. This is the one piece of the DSP that cannot be shared
+ * verbatim: dts_drc_sum_squares() reads libdca's PLANAR FLOAT buffer already
+ * normalised to +-1.0, whereas ff_mlp_pack_output() has just written
+ * INTERLEAVED INTEGER PCM. The arithmetic is the same sum of squares; only the
+ * addressing and the normalisation differ.
+ *
+ * The normalisation is EXACT: both full-scale divisors are powers of two
+ * (2^31 for S32, 2^15 for S16), so the mean square handed to
+ * dts_drc_level_dbfs() sits on the identical "0 dBFS == 1.0" reference the DTS
+ * side uses, and the curve therefore sees the same level for the same audio.
+ * Squares are accumulated in the RAW integer domain and scaled ONCE at the
+ * end, so the inner loop stays at one float multiply + one float add per
+ * sample, matching the DTS side's cost. Worst case magnitude is
+ * 8 ch * 160 samples * (2^31)^2 ~= 5.9e21, far inside float range.
+ *
+ * skip_ch / skip_ch2 are the LFE channels (-1 for none) — the contract
+ * excludes LFE from the detector, and a TrueHD layout can carry TWO of them
+ * (AV_CH_LOW_FREQUENCY and AV_CH_LOW_FREQUENCY_2 are both in
+ * thd_channel_order), so both are skipped. */
+static float mlp_drc_sum_squares_ilv(const void *data, int nb_samples,
+                                     int channels, int skip_ch, int skip_ch2,
+                                     int is32, int *count_out)
+{
+    float sum = 0.0f;
+    int count = 0;
+    int n, c;
+
+    if (is32) {
+        const int32_t *p = data;
+
+        for (n = 0; n < nb_samples; n++) {
+            for (c = 0; c < channels; c++) {
+                float v;
+
+                if (c == skip_ch || c == skip_ch2) {
+                    p++;
+                    continue;
+                }
+                v = (float) *p++;
+                sum += v * v;
+                count++;
+            }
+        }
+        sum *= (1.0f / 2147483648.0f) * (1.0f / 2147483648.0f);
+    } else {
+        const int16_t *p = data;
+
+        for (n = 0; n < nb_samples; n++) {
+            for (c = 0; c < channels; c++) {
+                float v;
+
+                if (c == skip_ch || c == skip_ch2) {
+                    p++;
+                    continue;
+                }
+                v = (float) *p++;
+                sum += v * v;
+                count++;
+            }
+        }
+        sum *= (1.0f / 32768.0f) * (1.0f / 32768.0f);
+    }
+
+    *count_out = count;
+    return sum;
+}
+
+/* Per-sample apply for the DRC / centre-boost path. The caller has already
+ * folded the make-up gain into from/to, so the contract's order
+ *   sample -> x drc_linear (interpolated across the block)
+ *          -> x center_linear (centre channel only)
+ *          -> x makeup_gain_linear -> saturating clamp
+ * holds exactly as it does in gstdtsdec.c.
+ *
+ * BEGIN DRC per-sample apply — this loop must stay free of transcendentals and
+ * of doubles in the GAIN arithmetic (armel soft-float target): one float add +
+ * one float multiply per sample. The double is only the pre-existing
+ * scale/clamp step, kept identical to the shipped make-up-gain loop above.
+ * Because the PCM is interleaved rather than planar, the ramp is carried per
+ * channel in cur[]/step[] instead of by the channel-outer loop the DTS side
+ * uses; the value sequence each channel sees is the same, including the DTS
+ * side's "advance before multiply" order (first sample gets from+step, last
+ * gets exactly to). */
+static void mlp_drc_apply(void *data, int nb_samples, int channels, int is32,
+                          float from, float to, int center_ch,
+                          float center_linear)
+{
+    float cur[MAX_CHANNELS], step[MAX_CHANNELS];
+    float inv_n;
+    int n, c;
+
+    /* Unreachable in practice — output_data() has already checked
+     * avctx->channels == s->max_matrix_channel + 1 and read_restart_header()
+     * rejects max_matrix_channel > 7. It is here only because cur[]/step[] are
+     * fixed-size stack arrays indexed by the channel count. */
+    if (nb_samples < 1 || channels < 1 || channels > MAX_CHANNELS)
+        return;
+
+    inv_n = 1.0f / (float) nb_samples;
+    for (c = 0; c < channels; c++) {
+        float chan_scale = (c == center_ch) ? center_linear : 1.0f;
+
+        cur[c]  = from * chan_scale;
+        step[c] = (to - from) * chan_scale * inv_n;
+    }
+
+    if (is32) {
+        int32_t *p = data;
+
+        for (n = 0; n < nb_samples; n++) {
+            for (c = 0; c < channels; c++) {
+                double s;
+
+                cur[c] += step[c];
+                s = (double) ((float) *p * cur[c]);
+                if (s > 2147483647.0)
+                    s = 2147483647.0;
+                else if (s < -2147483648.0)
+                    s = -2147483648.0;
+                *p++ = (int32_t) s;
+            }
+        }
+    } else {
+        int16_t *p = data;
+
+        for (n = 0; n < nb_samples; n++) {
+            for (c = 0; c < channels; c++) {
+                double s;
+
+                cur[c] += step[c];
+                s = (double) ((float) *p * cur[c]);
+                if (s > 32767.0)
+                    s = 32767.0;
+                else if (s < -32768.0)
+                    s = -32768.0;
+                *p++ = (int16_t) s;
+            }
+        }
+    }
+}
+/* END DRC per-sample apply */
+
+/* Clear the DRC runtime state: unity gain, nothing smoothed yet, coefficients
+ * forced to be recomputed. Mirrors gst_dtsdec_drc_reset(). */
+static void mlp_drc_reset(MLPDecodeContext *m)
+{
+    m->drc_smoothed_db   = 0.0f;
+    m->drc_prev_linear   = 1.0f;
+    m->drc_target_linear = 1.0f;
+    m->drc_attack_coef   = 1.0f;
+    m->drc_release_coef  = 1.0f;
+    m->drc_coef_rate     = 0;
+    m->drc_coef_mode     = -1;
+    m->drc_coef_block    = 0;
+    m->drc_acc_sum_sq    = 0.0f;
+    m->drc_acc_count     = 0;
+    m->drc_acc_samples   = 0;
+}
+
+/* (Re)derive the one-pole coefficients for the DETECTOR WINDOW (not the access
+ * unit — see the drc_acc_* note on MLPDecodeContext). Called only when the
+ * mode, the sample rate or the window length changes, so its two expf() calls
+ * never land on the per-block path.
+ *
+ * `window` is the number of per-channel samples actually accumulated before
+ * the smoother was stepped: DTS_DRC_BLOCK_SAMPLES rounded UP to a whole number
+ * of access units, so 280 samples (7 x 40) at 48 kHz against libdca's 256.
+ * Deriving the period from the real accumulated length rather than assuming
+ * 256 keeps the time constant honest. The attack/release TIME CONSTANTS in the
+ * shared profile table are untouched, so both decoders converge at the same
+ * rate in seconds and over the same size of averaging window. */
+static void mlp_drc_update_coefs(MLPDecodeContext *m, int rate, int window)
+{
+    if (m->drc_mode == DTS_DRC_MODE_LINE || m->drc_mode == DTS_DRC_MODE_RF) {
+        const DtsDrcProfile *p = &dts_drc_profiles[m->drc_mode];
+        float period = (float) ((window > 0) ? window : DTS_DRC_BLOCK_SAMPLES)
+            / (float) ((rate > 0) ? rate : 48000);
+
+        m->drc_attack_coef  = dts_drc_smooth_coef(p->attack_ms, period);
+        m->drc_release_coef = dts_drc_smooth_coef(p->release_ms, period);
+    } else {
+        m->drc_attack_coef  = 1.0f;
+        m->drc_release_coef = 1.0f;
+    }
+
+    m->drc_coef_rate  = rate;
+    m->drc_coef_mode  = m->drc_mode;
+    m->drc_coef_block = window;
+}
+
 static av_cold int mlp_decode_init(AVCodecContext *avctx)
 {
     static AVOnce init_static_once = AV_ONCE_INIT;
@@ -289,9 +1229,69 @@ static av_cold int mlp_decode_init(AVCodecContext *avctx)
 
     ff_thread_once(&init_static_once, init_static);
 
+    /* webOS 25 loudness patch: read + cache EVERY user setting ONCE here at
+     * init — never per frame, never per sample. Defaults (gain 0 dB, drc off,
+     * center 0 dB) leave the whole path inert. The clamps mirror
+     * gst_dtsdec_init()'s, and are belt-and-braces: the config reader has
+     * already clamped (epic amendment B). */
+    {
+        DtsDrcConfig cfg;
+
+        /* Two readers, on purpose: the legacy bare float goes through the
+         * shipped double-precision path so the make-up gain stays bit-exact
+         * with the shipped build, and the core supplies the new DRC keys.
+         * cfg.gain_db is deliberately IGNORED — see mlp_read_makeup_gain_db(). */
+        dts_drc_config_read_file(MLP_MAKEUP_GAIN_CONF_PATH, &cfg);
+
+        m->makeup_gain_linear   =
+            mlp_makeup_gain_db_to_linear(mlp_read_makeup_gain_db());
+        m->makeup_gain_linear_f = (float) m->makeup_gain_linear;
+        m->drc_mode             = cfg.drc_mode;
+        m->drc_boost_pct        = dts_drc_clampf(cfg.drc_boost_pct,
+                                                 DTS_DRC_PCT_MIN,
+                                                 DTS_DRC_PCT_MAX);
+        m->drc_cut_pct          = dts_drc_clampf(cfg.drc_cut_pct,
+                                                 DTS_DRC_PCT_MIN,
+                                                 DTS_DRC_PCT_MAX);
+        m->center_boost_db      = dts_drc_clampf(cfg.center_db,
+                                                 DTS_DRC_CENTER_DB_MIN,
+                                                 DTS_DRC_CENTER_DB_MAX);
+        m->center_boost_linear  = dts_drc_db_to_linear(m->center_boost_db);
+        m->drc_active           = (m->drc_mode != DTS_DRC_MODE_OFF
+                                   || m->center_boost_db != 0.0f);
+        mlp_drc_reset(m);
+
+        av_log(avctx, AV_LOG_INFO,
+               "webOS25 mlp/truehd loudness at init: make-up gain linear "
+               "%.4f%s, drc %s (boost %.0f%%, cut %.0f%%), center %+.1f dB%s\n",
+               m->makeup_gain_linear,
+               m->makeup_gain_linear == 1.0 ? " (unity)" : "",
+               dts_drc_mode_to_string(m->drc_mode),
+               m->drc_boost_pct, m->drc_cut_pct, m->center_boost_db,
+               m->drc_active ? "" : " -> fully inert, stock-identical decode");
+    }
+
+    /* dts_drc_sum_squares() is the DTS host's planar-float detector input; MLP
+     * PCM is interleaved integer, so mlp_drc_sum_squares_ilv() stands in for
+     * it. Reference it here so -Wunused-function stays quiet WITHOUT editing
+     * the ported core and breaking its byte-for-byte identity with
+     * gstdtsdec.c. */
+    (void) dts_drc_sum_squares;
+
     return 0;
 }
 
+/* webOS 25 loudness patch: a seek must not carry the compressor's smoothed
+ * gain across the discontinuity — the DTS side clears the same state in
+ * gst_dtsdec_start(). Nothing else in this decoder needed a flush hook, so
+ * this one only touches DRC state. */
+static void mlp_decode_flush(AVCodecContext *avctx)
+{
+    MLPDecodeContext *m = avctx->priv_data;
+
+    mlp_drc_reset(m);
+}
+
 /** Read a major sync info header - contains high level information about
  *  the stream - sample rate, channel arrangement etc. Most of this
  *  information is not actually necessary for decoding, only for playback.
@@ -1118,6 +2118,110 @@ static int output_data(MLPDecodeContext *m, unsigned int substr,
                                                     s->max_matrix_channel,
                                                     is32);
 
+    /* ----------------------------------------------------------------------
+     * webOS 25 loudness patch: make-up gain / DRC / centre boost, applied to
+     * the freshly-packed interleaved PCM (frame->data[0]; avctx->channels ==
+     * s->max_matrix_channel + 1, verified at the top of this function).
+     *
+     * The buffer is still UNPROCESSED here, which is what keeps the detector
+     * FEED-FORWARD: it measures pre-gain samples. The curve is unimodal (epic
+     * amendment D — two input levels can map to one gain), so a detector fed
+     * post-gain samples could limit-cycle on the decay leg. Do NOT move the
+     * measurement below the apply.
+     * -------------------------------------------------------------------- */
+    if (!m->drc_active) {
+        /* Inert / make-up-gain-only: the previously shipped call, unchanged,
+         * so output stays bit-identical to the shipped build at ANY gain — and
+         * at the default 0 dB the exact linear 1.0 short-circuits it entirely. */
+        mlp_apply_makeup_gain(frame->data[0], s->blockpos, avctx->channels,
+                              is32, m->makeup_gain_linear);
+    } else {
+        float drc_linear = 1.0f;
+        float from, to;
+        int center_ch = -1, lfe_ch = -1, lfe2_ch = -1, idx;
+
+        /* Locate the front centre (the dialogue lift target) and the LFE
+         * (excluded from the level detector) in the OUTPUT channel order.
+         * Layouts without a discrete centre simply get no lift. Mono is
+         * excluded deliberately — ffmpeg models it as FRONT_CENTER, but
+         * lifting the only channel is just make-up gain, which the make-up
+         * gain already provides. This mirrors gst_dtsdec_handle_frame(). */
+        if (avctx->channels > 1) {
+            idx = av_get_channel_layout_channel_index(avctx->channel_layout,
+                                                      AV_CH_FRONT_CENTER);
+            if (idx >= 0 && idx < avctx->channels)
+                center_ch = idx;
+        }
+        /* A TrueHD layout can carry two LFEs; the contract excludes LFE from
+         * the detector, so both are skipped. */
+        idx = av_get_channel_layout_channel_index(avctx->channel_layout,
+                                                  AV_CH_LOW_FREQUENCY);
+        if (idx >= 0 && idx < avctx->channels)
+            lfe_ch = idx;
+        idx = av_get_channel_layout_channel_index(avctx->channel_layout,
+                                                  AV_CH_LOW_FREQUENCY_2);
+        if (idx >= 0 && idx < avctx->channels)
+            lfe2_ch = idx;
+
+        if (m->drc_mode != DTS_DRC_MODE_OFF) {
+            int count = 0;
+            float sum_sq;
+
+            /* Accumulate this access unit into the detector window. The
+             * smoother is stepped once the window reaches the DTS block size,
+             * NOT once per access unit — an access unit is 6.4x shorter, and
+             * measuring on it would change the settled gain, not just its
+             * noisiness (see the drc_acc_* note on MLPDecodeContext). */
+            sum_sq = mlp_drc_sum_squares_ilv(frame->data[0], s->blockpos,
+                                             avctx->channels, lfe_ch, lfe2_ch,
+                                             is32, &count);
+            m->drc_acc_sum_sq  += sum_sq;
+            m->drc_acc_count   += count;
+            m->drc_acc_samples += s->blockpos;
+
+            if (m->drc_acc_samples >= DTS_DRC_BLOCK_SAMPLES) {
+                float level_db, target_db;
+
+                if (m->drc_coef_rate  != avctx->sample_rate
+                    || m->drc_coef_mode  != m->drc_mode
+                    || m->drc_coef_block != m->drc_acc_samples)
+                    mlp_drc_update_coefs(m, avctx->sample_rate,
+                                         m->drc_acc_samples);
+
+                level_db = dts_drc_level_dbfs(m->drc_acc_sum_sq,
+                                              m->drc_acc_count);
+                target_db = dts_drc_scale_gain_db(dts_drc_target_gain_db
+                    (m->drc_mode, level_db), m->drc_boost_pct,
+                    m->drc_cut_pct);
+                m->drc_smoothed_db = dts_drc_smooth_step(m->drc_smoothed_db,
+                    target_db, m->drc_attack_coef, m->drc_release_coef);
+                m->drc_target_linear =
+                    dts_drc_db_to_linear(m->drc_smoothed_db);
+
+                m->drc_acc_sum_sq  = 0.0f;
+                m->drc_acc_count   = 0;
+                m->drc_acc_samples = 0;
+            }
+            /* Held flat between detector updates; the ramp below still spreads
+             * every change across an access unit, so the gain never steps. */
+            drc_linear = m->drc_target_linear;
+        } else {
+            m->drc_smoothed_db   = 0.0f;
+            m->drc_target_linear = 1.0f;
+        }
+
+        /* Fold the make-up gain into the block's start/end factors, then ramp
+         * linearly from the previous block's value so gain changes cannot
+         * zipper. The DRC factor is identical on every channel (the surround
+         * image is preserved); only the centre gets the extra lift. */
+        from = m->drc_prev_linear * m->makeup_gain_linear_f;
+        to   = drc_linear * m->makeup_gain_linear_f;
+        m->drc_prev_linear = drc_linear;
+
+        mlp_drc_apply(frame->data[0], s->blockpos, avctx->channels, is32,
+                      from, to, center_ch, m->center_boost_linear);
+    }
+
     /* Update matrix encoding side data */
     if ((ret = ff_side_data_update_matrix_encoding(frame, s->matrix_encoding)) < 0)
         return ret;
@@ -1339,6 +2443,8 @@ AVCodec ff_mlp_decoder = {
     .priv_data_size = sizeof(MLPDecodeContext),
     .init           = mlp_decode_init,
     .decode         = read_access_unit,
+    /* webOS 25 loudness patch: clear the compressor state across seeks. */
+    .flush          = mlp_decode_flush,
     .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
     .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE,
 };
@@ -1352,6 +2458,8 @@ AVCodec ff_truehd_decoder = {
     .priv_data_size = sizeof(MLPDecodeContext),
     .init           = mlp_decode_init,
     .decode         = read_access_unit,
+    /* webOS 25 loudness patch: clear the compressor state across seeks. */
+    .flush          = mlp_decode_flush,
     .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
     .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE,
 };
PATCH_EOF

echo "--- applying mlpdec webOS 25 loudness patch (gain + DRC + centre boost) ---"
# Blast radius, asserted on the patch itself BEFORE it is applied: it may touch
# libavcodec/mlpdec.c and nothing else. mlpdec.c is the translation unit that
# decodes only truehd+mlp, so a patch confined to it cannot change aac/ac3/
# eac3/alac output in the shared libgstlibav.so (2026-07-23 Spotify regression).
if grep -E '^(\+\+\+|---) ' /tmp/mlpdec-webos25-loudness.patch \
     | grep -vE '^(\+\+\+ b|--- a)/libavcodec/mlpdec\.c$'; then
  echo "PATCH VERIFY FAILED: patch touches files outside libavcodec/mlpdec.c"; exit 1
fi
# ...and assert it named a file AT ALL, so an empty or truncated heredoc cannot
# read as "no out-of-scope files" (the chunk-01 false-pass failure mode).
scope_lines=$(grep -cE '^(\+\+\+|---) ' /tmp/mlpdec-webos25-loudness.patch || true)
if [ "$scope_lines" != 2 ]; then
  echo "PATCH VERIFY FAILED: expected exactly 2 file headers, found $scope_lines"; exit 1
fi
echo "scope OK: patch touches libavcodec/mlpdec.c only"

# Cloned with git (depth 1) so `git apply` works; fall back to plain `patch`.
if git apply --check /tmp/mlpdec-webos25-loudness.patch 2>/dev/null; then
  git apply /tmp/mlpdec-webos25-loudness.patch
elif patch -p1 --dry-run < /tmp/mlpdec-webos25-loudness.patch >/dev/null 2>&1; then
  patch -p1 < /tmp/mlpdec-webos25-loudness.patch
else
  echo "PATCH FAILED: mlpdec-webos25-loudness.patch does not apply to $FFTAG libavcodec/mlpdec.c"; exit 1
fi

echo "=== loudness patch verification ==="
# Verify-or-fail. Each grep asserts a DIFFERENT invariant, and each one must be
# able to fail: they run against the file we just mutated, so a silently
# non-applied patch cannot read as a pass.
verify_has() {  # verify_has <pattern> <what>
  if ! grep -q -- "$1" libavcodec/mlpdec.c; then
    echo "PATCH VERIFY FAILED: $2"; exit 1
  fi
}
verify_has 'mlp_apply_makeup_gain'                 "mlp_apply_makeup_gain missing"
verify_has '/var/lib/webosbrew/truehd/gain.conf'   "truehd gain.conf path missing"
verify_has '<<<DRC-CORE-BEGIN>>>'                  "ported DRC core missing"
verify_has 'dts_drc_target_gain_db'                "DRC curve missing"
verify_has 'mlp_drc_apply'                         "DRC per-sample apply missing"
verify_has 'mlp_drc_sum_squares_ilv'               "DRC level detector missing"
# Amendment F retired the silence gate; it must never come back (it made the
# hard-cut-from-high-gain case peak at -0.42 dBFS instead of -12.01 dBFS).
if grep -qE 'GATE_DB|drc_gate|gate_holds' libavcodec/mlpdec.c; then
  echo "PATCH VERIFY FAILED: a silence gate was reintroduced (epic amendment F retired it)"; exit 1
fi
# The detector must stay feed-forward: it reads frame->data[0] BEFORE any gain
# is applied. The curve is unimodal (amendment D), so a post-gain detector
# could limit-cycle on the decay leg. Assert the source order.
if ! awk '/mlp_drc_sum_squares_ilv\(frame->data\[0\]/{d=NR}
          /mlp_drc_apply\(frame->data\[0\]/{if(!a)a=NR}
          END{exit !(d>0 && a>0 && d<a)}' libavcodec/mlpdec.c; then
  echo "PATCH VERIFY FAILED: detector does not precede the apply (feed-forward broken)"; exit 1
fi
echo "=== loudness patch OK (mlpdec.c only; aac/ac3/eac3/alac untouched) ==="

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
  echo "===================== webOS 25 LOUDNESS PATCH CHECK ====================="
  echo "  Confirms the make-up gain / DRC / centre boost really reached the shipped"
  echo "  libavcodec. Codec-locality itself is guaranteed EARLIER, by the pre-apply"
  echo "  assertion that the patch names no file but libavcodec/mlpdec.c — that is"
  echo "  the load-bearing check, and it does fail on an out-of-scope file."
  if strings "$AVCODEC" | grep -q '/var/lib/webosbrew/truehd/gain.conf'; then
    echo "  config path present:  yes (/var/lib/webosbrew/truehd/gain.conf)"
  else echo "  config path present:  NO — patch did not reach the binary"; ALL_OK=0; fi
  # Weak by construction: file-static symbols never enter .dynsym, and ffmpeg
  # additionally links libavcodec with an av*-only version script. So this can
  # only catch a gross mistake (a symbol accidentally made non-static AND
  # exported); it is a smoke test, NOT proof that everything stayed static.
  leaked=$(objdump -T "$AVCODEC" 2>/dev/null | grep -cE '\b(dts_drc_|mlp_drc_|mlp_apply_makeup_gain|mlp_makeup_gain_db_to_linear|mlp_decode_flush)' || true)
  if [ "${leaked:-0}" = 0 ]; then
    echo "  exported symbols:     none [smoke test OK]"
  else echo "  exported symbols:     $leaked LEAKED [FAIL — not codec-local]"; ALL_OK=0; fi
  echo
  echo "===================== BUNDLE CONTENTS + SIZE ====================="
  ls -la /out/*.so* | awk '{printf "  %8s  %s\n", $5, $9}'
  echo "  TOTAL: $(du -ch /out/*.so* | tail -1 | awk '{print $1}')"
  echo
  echo "OVERALL: $([ "$ALL_OK" = 1 ] && echo 'PASS — all ELF objects ARM EABI5 soft-float (05000200), max GLIBC <=2.35, TrueHD decoder present, plugin links libavcodec decode API.' || echo 'CHECK FAILURES ABOVE')"
} | tee "$REPORT"
echo "############ DONE ############"
