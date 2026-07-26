/* GStreamer DTS decoder plugin based on libdtsdec
 * Copyright (C) 2004 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2009 Jan Schmidt <thaytan@noraisin.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/* ==========================================================================
 * webOS 25 DTS-restore patch (three functional changes vs. upstream)
 * --------------------------------------------------------------------------
 * Vendored from gst-plugins-bad 1.22.0 (ext/dts/gstdtsdec.c). Functional
 * changes from upstream:
 *
 * 1. The sink pad template caps below (the GST_STATIC_CAPS string on the
 *    "sink" pad).
 *
 * Upstream:
 *     GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts")
 * Patched:
 *     GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts; "
 *                      "audio/x-unknown, codec-id=(string)A_DTS")
 *
 * WHY: On webOS 25 (LG C5), matroskademux does NOT tag the DTS track as
 * audio/x-dts. LG ships no DTS decoder, so the MKV DTS track is re-tagged as
 * "audio/x-unknown, codec-id=(string)A_DTS" with the raw DTS bytes preserved.
 * By widening the sink caps to also advertise that exact media type,
 * decodebin/decproxy will autoplug THIS dtsdec directly onto LG's retagged
 * stream — no need to patch matroskademux or any LG library. The decoder
 * body is unchanged: it still parses/decodes the raw DTS elementary stream
 * via libdca and emits audio/x-raw.
 *
 * 2. A user-tunable "makeup-gain-db" property (float, default 0.0 dB =
 *    unity = exact no-op) applied in the float->S32 output conversion loop,
 *    BEFORE the existing integer clamp. The default value is read once at
 *    decoder init from /var/lib/webosbrew/dts25/gain.conf (a single ASCII
 *    dB float; missing/empty/unparseable -> 0.0 dB unity; clamped to
 *    [-20, +20] dB); an explicit property set (e.g. the app self-test)
 *    overrides the config-file value.
 *
 * WHY: DTS plays noticeably quieter than LG's native AAC/AC-3/Atmos decoders
 * on webOS 25 because they bake in dialnorm/DRC that this custom decoder
 * does not apply. See .orchestration/dts-loudness-makeup-gain/EPIC.md
 * (ADR-001) for the full rationale and config-file contract.
 *
 * 3. A signal-driven dynamic range compressor (Dolby Line/RF-style profile,
 *    scaled by boost%/cut% exactly like LG's DSP parameters) plus a separate
 *    front-centre "dialogue" boost, both applied in the same output loop and
 *    both OFF by default. The same gain.conf gained four optional key=value
 *    lines: drc=off|line|rf, drc_boost=0..100, drc_cut=0..100, center=-10..10.
 *    A bare float on its own line still parses as the make-up gain, so
 *    existing config files keep working unchanged.
 *
 * WHY: a fixed make-up gain raises everything equally, so it cannot fix the
 * dialogue-vs-effects balance that LG's DSP gets from Dolby Line-mode DRC on
 * native AC-3/E-AC-3/Atmos. See .orchestration/dts-truehd-drc/EPIC.md for the
 * evidence base (LG kernel driver parameter model) and the binding DSP
 * contract. The DSP math lives in the self-contained "DRC CORE" block below;
 * the TrueHD decoder (ffmpeg mlpdec.c) carries a byte-for-byte port of it.
 * ========================================================================== */

/**
 * SECTION:element-dtsdec
 * @title: dtsdec
 *
 * Digital Theatre System (DTS) audio decoder
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 dvdreadsrc title=1 ! mpegpsdemux ! dtsdec ! audioresample ! audioconvert ! alsasink
 * ]| Play a DTS audio track from a dvd.
 * |[
 * gst-launch-1.0 filesrc location=abc.dts ! dtsdec ! audioresample ! audioconvert ! alsasink
 * ]| Decode a standalone file and play it.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <gst/gst.h>
#include <gst/audio/audio.h>

#ifndef DTS_OLD
#include <dca.h>
#else
#include <dts.h>

typedef struct dts_state_s dca_state_t;
#define DCA_MONO DTS_MONO
#define DCA_CHANNEL DTS_CHANNEL
#define DCA_STEREO DTS_STEREO
#define DCA_STEREO_SUMDIFF DTS_STEREO_SUMDIFF
#define DCA_STEREO_TOTAL DTS_STEREO_TOTAL
#define DCA_3F DTS_3F
#define DCA_2F1R DTS_2F1R
#define DCA_3F1R DTS_3F1R
#define DCA_2F2R DTS_2F2R
#define DCA_3F2R DTS_3F2R
#define DCA_4F2R DTS_4F2R
#define DCA_DOLBY DTS_DOLBY
#define DCA_CHANNEL_MAX DTS_CHANNEL_MAX
#define DCA_CHANNEL_BITS DTS_CHANNEL_BITS
#define DCA_CHANNEL_MASK DTS_CHANNEL_MASK
#define DCA_LFE DTS_LFE
#define DCA_ADJUST_LEVEL DTS_ADJUST_LEVEL

#define dca_init dts_init
#define dca_syncinfo dts_syncinfo
#define dca_frame dts_frame
#define dca_dynrng dts_dynrng
#define dca_blocks_num dts_blocks_num
#define dca_block dts_block
#define dca_samples dts_samples
#define dca_free dts_free
#endif

#include "gstdtsdec.h"

#if HAVE_ORC
#include <orc/orc.h>
#endif

#if defined(LIBDTS_FIXED) || defined(LIBDCA_FIXED)
#define SAMPLE_WIDTH 16
#define SAMPLE_FORMAT GST_AUDIO_NE(S16)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_S16
#elif defined (LIBDTS_DOUBLE) || defined(LIBDCA_DOUBLE)
#define SAMPLE_WIDTH 64
#define SAMPLE_FORMAT GST_AUDIO_NE(F64)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_F64
#else
/* webOS: LG's audiosink accepts only integer PCM (no F32/F64). libdca decodes
 * to float; we emit S32LE (native-endian S32 on the LE TV) by converting in the
 * output loop below. Width stays 32 bits, so buffer sizing is unchanged. */
#define SAMPLE_WIDTH 32
#define SAMPLE_FORMAT GST_AUDIO_NE(S32)
#define SAMPLE_TYPE GST_AUDIO_FORMAT_S32
#endif

GST_DEBUG_CATEGORY_STATIC (dtsdec_debug);
#define GST_CAT_DEFAULT (dtsdec_debug)

enum
{
  PROP_0,
  PROP_DRC,
  PROP_MAKEUP_GAIN_DB,
  PROP_DRC_MODE,
  PROP_DRC_BOOST,
  PROP_DRC_CUT,
  PROP_CENTER_BOOST_DB
};

/* webOS 25 patch: on-device config file (host-specific, so it deliberately
 * lives OUTSIDE the portable DRC CORE block below). */
#define DTS_MAKEUP_GAIN_CONF_PATH "/var/lib/webosbrew/dts25/gain.conf"

/* webOS 25 patch: the DRC CORE parses ASCII floats through this hook so it
 * stays locale-independent without depending on GLib. Inside the plugin that
 * means GLib's g_ascii_strtod(); the host unit test and the ffmpeg port
 * substitute the C library's strtod() in the "C" locale. */
#define DTS_DRC_STRTOD(nptr, endptr) g_ascii_strtod ((nptr), (endptr))

/* The DRC detector reads libdca's planar output buffer through
 * dts_drc_sum_squares (const float *). libdca's default sample_t IS float;
 * the fixed-point and double builds are not part of the webOS 25 build
 * (build.sh defines neither macro). Fail loudly rather than misread samples. */
#if defined(LIBDTS_FIXED) || defined(LIBDCA_FIXED) || \
    defined(LIBDTS_DOUBLE) || defined(LIBDCA_DOUBLE)
#error "webOS 25 dtsdec DRC requires the default libdca build (sample_t == float)"
#endif

/*<<<DRC-CORE-BEGIN>>>*/
/* ==========================================================================
 * BEGIN DRC CORE
 * --------------------------------------------------------------------------
 * REFERENCE IMPLEMENTATION of the epic's DSP + config contract
 * (.orchestration/dts-truehd-drc/EPIC.md). Everything between the BEGIN/END
 * markers is deliberately dependency-free: plain C using only <math.h>,
 * <stdio.h>, <stdlib.h> and <string.h>, no GLib/GStreamer/libdca types. That
 * lets it be lifted verbatim into ffmpeg's mlpdec.c for the TrueHD decoder,
 * and lets webos25/restore/src/test/run-tests.sh extract everything between
 * the DRC-CORE-BEGIN / DRC-CORE-END marker comments and unit-test the exact
 * shipped code instead of a copy of it. Keep the two decoders in sync: change
 * the math HERE, then re-port.
 *
 * Cost model (target is armel soft-float, 5.1 @ 48 kHz):
 *   per block (256 samples): 1x log10f (detector) + 1x powf (dB->linear),
 *                            plus 2x expf only when mode/sample-rate changes.
 *   per sample:              1 float add (gain ramp) + 1 float multiply.
 * No transcendental and no double ever enters the per-sample path.
 * ========================================================================== */

/* ---- DRC modes (mirror LG's LX_AUD_DECODER_DRC_{OFF,LINE,RF}) ---------- */
#define DTS_DRC_MODE_OFF   0
#define DTS_DRC_MODE_LINE  1
#define DTS_DRC_MODE_RF    2

/* ---- ALL TUNABLE CONSTANTS LIVE IN THIS ONE BLOCK ---------------------- */

/* Decoder block size the detector/smoother run at (one libdca dca_block).
 * PORTING NOTE: the host code below this block still writes 256 literally
 * where upstream gstdtsdec.c already hardcoded it (buffer sizing, the output
 * loop). The MLP block size is NOT 256, so the mlpdec.c port must route every
 * one of those uses through this macro. */
#define DTS_DRC_BLOCK_SAMPLES 256

/* Detector output bounds. The floor keeps digital silence off -inf dBFS; the
 * ceiling bounds an over-range or non-finite block. Both matter for more than
 * tidiness: the smoother is stateful, so a single NaN or +inf level would
 * poison drc_smoothed_db for the rest of the stream (only _start() clears it)
 * and every later sample would convert from a NaN double. */
#define DTS_DRC_LEVEL_FLOOR_DB (-90.0f)
#define DTS_DRC_LEVEL_CEIL_DB (20.0f)

/* NOTE — there is deliberately NO silence gate here (epic amendment F retired
 * the one that used to live at this spot, and amendment E with it). Every
 * job it did is now covered elsewhere, and it made the worst case worse:
 *   - wind-up over quiet passages: handled by the boost decay in the curve
 *     below, which already targets 0 dB at -85 dBFS and lower;
 *   - non-finite levels: handled by dts_drc_level_dbfs()'s floor, which maps
 *     NaN to -90 dBFS (hence 0 dB of gain) before anything else sees it;
 *   - hard cut from an established high gain: a gate actively HURT this. It
 *     froze the pre-cut gain, so the next cue peaked at -0.42 dBFS — against
 *     -12.01 dBFS with no gate. Releasing normally is the correct response.
 * Do not reintroduce one in the mlpdec.c port. */

/* Config value ranges + defaults (epic "Config contract"). */
#define DTS_MAKEUP_GAIN_DB_MIN (-20.0f)
#define DTS_MAKEUP_GAIN_DB_MAX (20.0f)
#define DTS_DRC_PCT_MIN (0.0f)
#define DTS_DRC_PCT_MAX (100.0f)
#define DTS_DRC_CENTER_DB_MIN (-10.0f)
#define DTS_DRC_CENTER_DB_MAX (10.0f)

#define DTS_DRC_DEFAULT_GAIN_DB (0.0f)
#define DTS_DRC_DEFAULT_MODE DTS_DRC_MODE_OFF
#define DTS_DRC_DEFAULT_BOOST_PCT (100.0f)
#define DTS_DRC_DEFAULT_CUT_PCT (100.0f)
#define DTS_DRC_DEFAULT_CENTER_DB (0.0f)

/* Static compression curve + smoothing, one profile per mode.
 *
 * The shape follows the publicly documented structure of Dolby's Line/RF
 * profiles (null band, 2:1 early boost/cut regions, a steep max-boost region
 * at the bottom and a 20:1 limiting region at the top). These are a
 * documented-style APPROXIMATION chosen to mirror LG's DSP behaviour — they
 * are NOT Dolby's proprietary tables, and they are meant to be retuned here.
 *
 * Breakpoints, referenced to dBFS (from the epic's DSP contract). Read the
 * boost side left-to-right as rising level: the boost decays in from 0 dB at
 * -85, peaks, then falls away through the null band into the cuts.
 *   line: 0 dB <= -85 | decay -85..-50.5 | PEAK +12 at -50.5
 *         | 5:1 -50.5..-43 | 2:1 boost -43..-31 | null -31..-20
 *         | 2:1 cut -20..-10 | 20:1 cut > -10
 *   rf:   0 dB <= -85 | decay -85..-55.5 | PEAK +16 at -55.5
 *         | 5:1 -55.5..-43 | 2:1 boost -43..-31 | null -31..-24
 *         | 2:1 cut -24..-14 | 20:1 cut > -14
 * The two peak levels are derived, not stored — see the amendment D note below.
 *
 * maxboost_ratio is 5:1 (epic amendment A). The epic's original table gave the
 * region below -43 dBFS only as "boost, capped at +N dB" with no ratio, and
 * the 2:1 segment above it reaches just +6 dB at -43, so a steeper ratio is
 * needed for the cap to be reachable at all. 5:1 is a project choice, not a
 * Dolby figure; it keeps the curve continuous with the 2:1 segment.
 *
 * BOOST DECAY (epic amendment D) — the boost region PEAKS and then declines
 * linearly back to 0 dB at boost_zero_db, instead of holding the cap all the
 * way down to the detector floor:
 *
 *   line: +12 dB peak at -50.5 dBFS -> 0 dB at -85 dBFS
 *   rf:   +16 dB peak at -55.5 dBFS -> 0 dB at -85 dBFS
 *
 * Both peak levels are DERIVED, not stored: they are simply where the 5:1
 * ramp reaches max_boost_db, so retuning the cap or the ratio moves them
 * automatically and the curve stays continuous by construction.
 *
 * WHY the decay: material below roughly -60 dBFS is room tone, tape hiss and
 * dither — amplifying that by 12-20 dB was never the intent, and a plateau
 * held all the way down to the detector floor meant a slow fade-out wound the
 * gain up to the cap, so the next loud cue swelled and clipped. With the decay,
 * quiet-but-real material still gets the full boost while near-silence returns
 * toward unity, so a fade cannot wind up. This is also the ONLY thing standing
 * between a quiet passage and a swollen cue — see the amendment F note above.
 * rf's cap is +16 rather than +20 for the same reason: +20 dB was the dominant
 * term in the worst-case clip and is more than this use case needs.
 *
 * Retune here — and re-port to mlpdec.c if you do. TWO INVARIANTS keep the
 * curve continuous; both are easy to break by eye, so they are spelled out:
 *
 *   1. maxboost_ratio MUST be > 1. At exactly 1 the rise slope is 0 and the
 *      peak derivation divides by zero. The OFF row below holds ratio 1.0 and
 *      is safe ONLY because dts_drc_target_gain_db() returns early for any
 *      mode that is not LINE or RF — a port MUST keep that guard.
 *   2. max_boost_db must stay inside
 *        knee_boost < max_boost_db
 *                   < knee_boost + (maxboost_knee_db - boost_zero_db) * slope
 *      where knee_boost is the +6 dB the 2:1 segment reaches at -43 dBFS and
 *      slope is 1 - 1/maxboost_ratio. With the pinned constants that is
 *      6 < cap < 39.6 dB. Below it, the cap sits under the knee and the curve
 *      steps at -43; above it, the peak falls past boost_zero_db, the decay
 *      leg vanishes and the curve steps at -85.
 *
 * The detector MUST stay feed-forward. This curve is unimodal, so two input
 * levels can map to the same gain; that is harmless here because the detector
 * reads the decoded samples before any gain is applied. A port that measured
 * post-gain samples could limit-cycle on the decay leg.
 *
 * Smoothing time constants (one-pole, dB domain, per block):
 *   line: attack 10 ms / release 250 ms;  rf: attack 5 ms / release 150 ms. */
typedef struct
{
  float boost_knee_db;          /* lower edge of the null band            */
  float boost_ratio;            /* n:1 over maxboost_knee_db..boost_knee_db */
  float maxboost_knee_db;       /* below this the steeper max-boost region */
  float maxboost_ratio;
  float max_boost_db;           /* peak positive gain (cap)               */
  float boost_zero_db;          /* boost decays back to 0 dB here         */
  float cut_knee_db;            /* upper edge of the null band            */
  float cut_ratio;              /* n:1 over cut_knee_db..hardcut_knee_db  */
  float hardcut_knee_db;
  float hardcut_ratio;          /* n:1 above hardcut_knee_db (limiting)   */
  float attack_ms;
  float release_ms;
} DtsDrcProfile;

static const DtsDrcProfile dts_drc_profiles[3] = {
  /* [DTS_DRC_MODE_OFF] never evaluated; kept so the array is mode-indexed. */
  {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
  /* [DTS_DRC_MODE_LINE] */
  {-31.0f, 2.0f, -43.0f, 5.0f, 12.0f, -85.0f, -20.0f, 2.0f, -10.0f, 20.0f,
      10.0f, 250.0f},
  /* [DTS_DRC_MODE_RF] */
  {-31.0f, 2.0f, -43.0f, 5.0f, 16.0f, -85.0f, -24.0f, 2.0f, -14.0f, 20.0f,
      5.0f, 150.0f}
};

/* ---- END OF TUNABLE CONSTANTS ------------------------------------------ */

/* Locale-independent ASCII float parse; see the DTS_DRC_STRTOD note above. */
#ifndef DTS_DRC_STRTOD
#define DTS_DRC_STRTOD(nptr, endptr) strtod ((nptr), (endptr))
#endif

/* User settings as read from the config file / GObject properties. */
typedef struct
{
  float gain_db;                /* make-up gain, dB (legacy bare float)   */
  int drc_mode;                 /* DTS_DRC_MODE_*                         */
  float drc_boost_pct;          /* 0..100                                 */
  float drc_cut_pct;            /* 0..100                                 */
  float center_db;              /* -10..+10 dB                            */
} DtsDrcConfig;

static float
dts_drc_clampf (float v, float lo, float hi)
{
  if (v > hi)
    return hi;
  if (v < lo)
    return lo;
  return v;
}

/* ASCII-only, locale-independent case-insensitive string equality. */
static int
dts_drc_ascii_ieq (const char *a, const char *b)
{
  while (*a != '\0' && *b != '\0') {
    char ca = *a++;
    char cb = *b++;

    if (ca >= 'A' && ca <= 'Z')
      ca = (char) (ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (char) (cb - 'A' + 'a');
    if (ca != cb)
      return 0;
  }
  return (*a == '\0' && *b == '\0');
}

static int
dts_drc_mode_from_string (const char *s)
{
  if (s != NULL) {
    if (dts_drc_ascii_ieq (s, "line"))
      return DTS_DRC_MODE_LINE;
    if (dts_drc_ascii_ieq (s, "rf"))
      return DTS_DRC_MODE_RF;
  }
  /* "off" and anything unrecognised -> the default, which is off. */
  return DTS_DRC_MODE_OFF;
}

static const char *
dts_drc_mode_to_string (int mode)
{
  if (mode == DTS_DRC_MODE_LINE)
    return "line";
  if (mode == DTS_DRC_MODE_RF)
    return "rf";
  return "off";
}

/* ------------------------------------------------------------------------
 * 1. Level detector — per block, full-range channels only (LFE excluded).
 * ---------------------------------------------------------------------- */

/* Sum of squares over a planar [nch][nsamples] float buffer, skipping channel
 * skip_ch (pass -1 for none — that is how LFE is kept out of the detector).
 * *count_out receives the number of samples actually summed. */
static float
dts_drc_sum_squares (const float *planar, int nch, int nsamples, int skip_ch,
    int *count_out)
{
  float sum = 0.0f;
  int count = 0;
  int c, n;

  for (c = 0; c < nch; c++) {
    const float *p;

    if (c == skip_ch)
      continue;
    p = planar + c * nsamples;
    for (n = 0; n < nsamples; n++)
      sum += p[n] * p[n];
    count += nsamples;
  }

  *count_out = count;
  return sum;
}

/* Block RMS as dBFS. Uses 10*log10(mean square) == 20*log10(rms), so this
 * costs ONE log10f per block and no sqrtf. Floored at DTS_DRC_LEVEL_FLOOR_DB
 * so digital silence cannot produce -inf. */
static float
dts_drc_level_dbfs (float sum_sq, int count)
{
  float mean_sq, level_db;

  if (count <= 0 || sum_sq <= 0.0f)
    return DTS_DRC_LEVEL_FLOOR_DB;

  mean_sq = sum_sq / (float) count;
  level_db = 10.0f * log10f (mean_sq);

  /* Bound the result on BOTH sides, and note the inverted comparison: it is
   * what makes this NaN-safe. `NaN < floor` is false, so the obvious form
   * would return NaN, and because the smoother is stateful a single bad
   * sample would then poison drc_smoothed_db for the whole stream. */
  if (!(level_db > DTS_DRC_LEVEL_FLOOR_DB))
    return DTS_DRC_LEVEL_FLOOR_DB;
  return (level_db > DTS_DRC_LEVEL_CEIL_DB) ? DTS_DRC_LEVEL_CEIL_DB :
      level_db;
}

/* ------------------------------------------------------------------------
 * 2. Static curve — input level (dBFS) -> target gain (dB).
 * ---------------------------------------------------------------------- */
static float
dts_drc_target_gain_db (int mode, float level_dbfs)
{
  const DtsDrcProfile *p;
  float gain_db;

  if (mode != DTS_DRC_MODE_LINE && mode != DTS_DRC_MODE_RF)
    return 0.0f;
  p = &dts_drc_profiles[mode];

  if (level_dbfs > p->hardcut_knee_db) {
    /* limiting region: full 2:1 cut across the early-cut band, then n:1 */
    gain_db = -((p->hardcut_knee_db - p->cut_knee_db)
        * (1.0f - 1.0f / p->cut_ratio)
        + (level_dbfs - p->hardcut_knee_db)
        * (1.0f - 1.0f / p->hardcut_ratio));
  } else if (level_dbfs > p->cut_knee_db) {
    /* early-cut region */
    gain_db = -((level_dbfs - p->cut_knee_db) * (1.0f - 1.0f / p->cut_ratio));
  } else if (level_dbfs >= p->boost_knee_db) {
    /* null band — the whole point of the profile: leave dialogue alone */
    gain_db = 0.0f;
  } else if (level_dbfs >= p->maxboost_knee_db) {
    /* boost region */
    gain_db = (p->boost_knee_db - level_dbfs) * (1.0f - 1.0f / p->boost_ratio);
  } else {
    /* Max-boost region: rise at maxboost_ratio up to the cap, then DECAY
     * linearly back to 0 dB at boost_zero_db (amendment D). */
    float knee_boost = (p->boost_knee_db - p->maxboost_knee_db)
        * (1.0f - 1.0f / p->boost_ratio);
    float rise_slope = 1.0f - 1.0f / p->maxboost_ratio;
    /* Where the rise reaches the cap. Derived, so the curve is continuous by
     * construction however the cap or the ratio is retuned. */
    float peak_level_db = p->maxboost_knee_db
        - (p->max_boost_db - knee_boost) / rise_slope;

    if (level_dbfs >= peak_level_db) {
      gain_db = knee_boost + (p->maxboost_knee_db - level_dbfs) * rise_slope;
      if (gain_db > p->max_boost_db)
        gain_db = p->max_boost_db;
    } else if (level_dbfs > p->boost_zero_db) {
      /* Decay leg. Reaching this branch requires
       * boost_zero_db < level_dbfs < peak_level_db, which already implies
       * peak_level_db > boost_zero_db — so the divisor cannot be zero. */
      gain_db = p->max_boost_db * (level_dbfs - p->boost_zero_db)
          / (peak_level_db - p->boost_zero_db);
    } else {
      gain_db = 0.0f;
    }
  }

  return gain_db;
}

/* ------------------------------------------------------------------------
 * 3. Scale by boost%/cut% — mirrors LG's drc_boost_scl_factor /
 *    drc_cut_scl_factor (both 0..100).
 * ---------------------------------------------------------------------- */
static float
dts_drc_scale_gain_db (float gain_db, float boost_pct, float cut_pct)
{
  if (gain_db > 0.0f)
    return gain_db * (boost_pct * 0.01f);
  return gain_db * (cut_pct * 0.01f);
}

/* ------------------------------------------------------------------------
 * 4. Smoothing — one-pole in the dB domain, stepped once per block.
 * ---------------------------------------------------------------------- */

/* One-pole coefficient for a time constant of tau_ms, stepped every
 * block_period_s seconds. Only ever called when mode or sample rate changes,
 * so its expf() is not on the per-block path. */
static float
dts_drc_smooth_coef (float tau_ms, float block_period_s)
{
  float tau_s = tau_ms * 0.001f;

  if (tau_s <= 0.0f || block_period_s <= 0.0f)
    return 1.0f;
  return 1.0f - expf (-block_period_s / tau_s);
}

/* Attack when the target gain falls (signal got louder), release when it
 * rises. Pure arithmetic — no transcendental. */
static float
dts_drc_smooth_step (float smoothed_db, float target_db, float attack_coef,
    float release_coef)
{
  float coef = (target_db < smoothed_db) ? attack_coef : release_coef;

  return smoothed_db + coef * (target_db - smoothed_db);
}

/* dB -> linear, once per block. Exactly 1.0f at 0 dB (no powf rounding), which
 * is what makes the "no DRC" state an exact multiply-by-one. */
static float
dts_drc_db_to_linear (float db)
{
  return (db == 0.0f) ? 1.0f : powf (10.0f, db * (1.0f / 20.0f));
}

/* ------------------------------------------------------------------------
 * 5. Config file — epic "Config contract".
 *    - a bare float on its own line  -> make-up gain dB (legacy format)
 *    - key=value lines               -> drc / drc_boost / drc_cut / center
 *    - '#' comments and blank lines ignored, unknown keys ignored
 *    - unparseable value -> that key's default; out-of-range -> clamped
 *    - a missing or unreadable file is not an error: defaults are used
 * ---------------------------------------------------------------------- */
static void
dts_drc_config_defaults (DtsDrcConfig * cfg)
{
  cfg->gain_db = DTS_DRC_DEFAULT_GAIN_DB;
  cfg->drc_mode = DTS_DRC_DEFAULT_MODE;
  cfg->drc_boost_pct = DTS_DRC_DEFAULT_BOOST_PCT;
  cfg->drc_cut_pct = DTS_DRC_DEFAULT_CUT_PCT;
  cfg->center_db = DTS_DRC_DEFAULT_CENTER_DB;
}

static char *
dts_drc_lstrip (char *s)
{
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}

static void
dts_drc_rstrip (char *s)
{
  size_t n = strlen (s);

  while (n > 0) {
    char c = s[n - 1];

    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    s[--n] = '\0';
  }
}

/* 1 + *out set (clamped) when val starts with a parseable ASCII float,
 * 0 otherwise (caller keeps that setting's default). */
static int
dts_drc_parse_clamped (const char *val, float lo, float hi, float *out)
{
  char *end = NULL;
  double parsed = DTS_DRC_STRTOD (val, &end);

  if (end == val)
    return 0;
  *out = dts_drc_clampf ((float) parsed, lo, hi);
  return 1;
}

/* Apply one config line to cfg. *have_gain guards the legacy bare float so
 * the FIRST one wins (matching the pre-DRC reader, which stopped at it). */
static void
dts_drc_config_parse_line (DtsDrcConfig * cfg, char *line, int *have_gain)
{
  char *p, *eq, *key, *val;

  p = dts_drc_lstrip (line);
  if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
    return;

  eq = strchr (p, '=');
  if (eq != NULL) {
    key = p;
    *eq = '\0';
    val = dts_drc_lstrip (eq + 1);
    dts_drc_rstrip (key);
    dts_drc_rstrip (val);

    if (dts_drc_ascii_ieq (key, "drc")) {
      cfg->drc_mode = dts_drc_mode_from_string (val);
    } else if (dts_drc_ascii_ieq (key, "drc_boost")) {
      if (!dts_drc_parse_clamped (val, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX,
              &cfg->drc_boost_pct))
        cfg->drc_boost_pct = DTS_DRC_DEFAULT_BOOST_PCT;
    } else if (dts_drc_ascii_ieq (key, "drc_cut")) {
      if (!dts_drc_parse_clamped (val, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX,
              &cfg->drc_cut_pct))
        cfg->drc_cut_pct = DTS_DRC_DEFAULT_CUT_PCT;
    } else if (dts_drc_ascii_ieq (key, "center")) {
      if (!dts_drc_parse_clamped (val, DTS_DRC_CENTER_DB_MIN,
              DTS_DRC_CENTER_DB_MAX, &cfg->center_db))
        cfg->center_db = DTS_DRC_DEFAULT_CENTER_DB;
    }
    /* unknown key -> ignored, so newer config files stay readable */
    return;
  }

  if (!*have_gain && dts_drc_parse_clamped (p, DTS_MAKEUP_GAIN_DB_MIN,
          DTS_MAKEUP_GAIN_DB_MAX, &cfg->gain_db))
    *have_gain = 1;
}

/* Never fails: cfg always comes back fully populated. */
static void
dts_drc_config_read_file (const char *path, DtsDrcConfig * cfg)
{
  FILE *f;
  char line[256];
  int have_gain = 0;

  dts_drc_config_defaults (cfg);

  f = fopen (path, "r");
  if (f == NULL)
    return;

  while (fgets (line, sizeof (line), f) != NULL)
    dts_drc_config_parse_line (cfg, line, &have_gain);

  fclose (f);
}

/* ==========================================================================
 * END DRC CORE
 * ========================================================================== */
/*<<<DRC-CORE-END>>>*/

/* webOS 25 patch: sink caps widened to also accept LG's retagged raw DTS
 * ("audio/x-unknown, codec-id=(string)A_DTS"). See file header for rationale. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts; audio/x-unknown, codec-id=(string)A_DTS")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " SAMPLE_FORMAT ", "
        "layout = (string) interleaved, "
        "rate = (int) [ 4000, 96000 ], " "channels = (int) [ 1, 6 ]")
    );



static gboolean gst_dtsdec_start (GstAudioDecoder * dec);
static gboolean gst_dtsdec_stop (GstAudioDecoder * dec);
static gboolean gst_dtsdec_set_format (GstAudioDecoder * bdec, GstCaps * caps);
static GstFlowReturn gst_dtsdec_parse (GstAudioDecoder * dec,
    GstAdapter * adapter, gint * offset, gint * length);
static GstFlowReturn gst_dtsdec_handle_frame (GstAudioDecoder * dec,
    GstBuffer * buffer);

static GstFlowReturn gst_dtsdec_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);

static void gst_dtsdec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_dtsdec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean dtsdec_element_init (GstPlugin * plugin);

/* webOS 25 patch: make-up gain / DRC / centre-boost helpers (see file header
 * + EPIC). The DSP itself lives in the DRC CORE block above; these only bind
 * it to the GObject instance. */
static void gst_dtsdec_apply_makeup_gain_db (GstDtsDec * dts, gfloat gain_db);
static void gst_dtsdec_apply_center_boost_db (GstDtsDec * dts, gfloat boost_db);
static void gst_dtsdec_drc_reset (GstDtsDec * dts);
static void gst_dtsdec_drc_update_coefs (GstDtsDec * dts);

G_DEFINE_TYPE (GstDtsDec, gst_dtsdec, GST_TYPE_AUDIO_DECODER);
GST_ELEMENT_REGISTER_DEFINE_CUSTOM (dtsdec, dtsdec_element_init);

static void
gst_dtsdec_class_init (GstDtsDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstAudioDecoderClass *gstbase_class;
  guint cpuflags;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbase_class = (GstAudioDecoderClass *) klass;

  gobject_class->set_property = gst_dtsdec_set_property;
  gobject_class->get_property = gst_dtsdec_get_property;

  gst_element_class_add_static_pad_template (gstelement_class, &sink_factory);
  gst_element_class_add_static_pad_template (gstelement_class, &src_factory);
  gst_element_class_set_static_metadata (gstelement_class, "DTS audio decoder",
      "Codec/Decoder/Audio",
      "Decodes DTS audio streams",
      "Jan Schmidt <thaytan@noraisin.net>, "
      "Ronald Bultje <rbultje@ronald.bitfreak.net>");

  gstbase_class->start = GST_DEBUG_FUNCPTR (gst_dtsdec_start);
  gstbase_class->stop = GST_DEBUG_FUNCPTR (gst_dtsdec_stop);
  gstbase_class->set_format = GST_DEBUG_FUNCPTR (gst_dtsdec_set_format);
  gstbase_class->parse = GST_DEBUG_FUNCPTR (gst_dtsdec_parse);
  gstbase_class->handle_frame = GST_DEBUG_FUNCPTR (gst_dtsdec_handle_frame);

  /**
   * GstDtsDec::drc
   *
   * Set to true to apply the recommended DTS dynamic range compression
   * to the audio stream. Dynamic range compression makes loud sounds
   * softer and soft sounds louder, so you can more easily listen
   * to the stream without disturbing other people.
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DRC,
      g_param_spec_boolean ("drc", "Dynamic Range Compression",
          "Use Dynamic Range Compression", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstDtsDec::makeup-gain-db
   *
   * webOS 25 patch: user-tunable make-up gain, in dB, applied to the
   * decoded PCM output (before the final S32 clamp). Default 0.0 dB is
   * unity — an exact no-op matching upstream behaviour. The default is
   * read once at decoder init from /var/lib/webosbrew/dts25/gain.conf;
   * setting this property explicitly (e.g. the app self-test) overrides
   * that file value. Clamped to [-20.0, +20.0] dB.
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_MAKEUP_GAIN_DB,
      g_param_spec_float ("makeup-gain-db", "Make-up Gain (dB)",
          "User-tunable make-up gain in dB applied to decoded PCM output",
          DTS_MAKEUP_GAIN_DB_MIN, DTS_MAKEUP_GAIN_DB_MAX, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstDtsDec::drc-mode
   *
   * webOS 25 patch: dynamic range compression profile — "off" (default),
   * "line" (Dolby Line-mode-style, what LG's DSP applies to native Dolby
   * streams) or "rf" (heavier, night-listening). Unrecognised values mean
   * "off". Defaults come from /var/lib/webosbrew/dts25/gain.conf ("drc=");
   * setting this property overrides the file value.
   *
   * Note this is distinct from #GstDtsDec:drc, which only toggles libdca's
   * own in-stream dynamic-range metadata.
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DRC_MODE,
      g_param_spec_string ("drc-mode", "DRC Mode",
          "Dynamic range compression profile: \"off\", \"line\" or \"rf\"",
          "off", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstDtsDec::drc-boost
   *
   * webOS 25 patch: percentage of the DRC curve's boost (positive gain) that
   * is actually applied — mirrors LG's drc_boost_scl_factor. 0 disables
   * boosting, 100 (default) applies the full curve. Config key "drc_boost".
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DRC_BOOST,
      g_param_spec_float ("drc-boost", "DRC Boost Scale (%)",
          "Percentage of the DRC curve's boost that is applied",
          DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX, DTS_DRC_DEFAULT_BOOST_PCT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstDtsDec::drc-cut
   *
   * webOS 25 patch: percentage of the DRC curve's cut (negative gain) that is
   * actually applied — mirrors LG's drc_cut_scl_factor. 0 disables cutting,
   * 100 (default) applies the full curve. Config key "drc_cut".
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_DRC_CUT,
      g_param_spec_float ("drc-cut", "DRC Cut Scale (%)",
          "Percentage of the DRC curve's cut that is applied",
          DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX, DTS_DRC_DEFAULT_CUT_PCT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstDtsDec::center-boost-db
   *
   * webOS 25 patch: extra gain in dB applied to the front-centre channel only
   * (the dialogue lift). Default 0.0 dB is an exact no-op. Has no effect on
   * layouts without a discrete front-centre channel — stereo, 2F1R, 2F2R, and
   * 4F2R (which has FRONT_LEFT/RIGHT_OF_CENTER but no FRONT_CENTER). Mono is
   * deliberately excluded too: lifting the only channel is just make-up gain,
   * which #GstDtsDec:makeup-gain-db already provides. Config key "center".
   */
  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_CENTER_BOOST_DB,
      g_param_spec_float ("center-boost-db", "Centre Boost (dB)",
          "Extra gain in dB applied to the front-centre (dialogue) channel",
          DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX,
          DTS_DRC_DEFAULT_CENTER_DB,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  klass->dts_cpuflags = 0;

#if HAVE_ORC
  cpuflags = orc_target_get_default_flags (orc_target_get_by_name ("mmx"));
  if (cpuflags & ORC_TARGET_MMX_MMX)
    klass->dts_cpuflags |= MM_ACCEL_X86_MMX;
  if (cpuflags & ORC_TARGET_MMX_3DNOW)
    klass->dts_cpuflags |= MM_ACCEL_X86_3DNOW;
  if (cpuflags & ORC_TARGET_MMX_MMXEXT)
    klass->dts_cpuflags |= MM_ACCEL_X86_MMXEXT;
#else
  cpuflags = 0;
  klass->dts_cpuflags = 0;
#endif

  GST_LOG ("CPU flags: dts=%08x, orc=%08x", klass->dts_cpuflags, cpuflags);
}

/* webOS 25 patch: clamp + cache the linear multiplier for a dB gain value.
 * gain_db == 0.0 always yields an exact linear 1.0 (no powf rounding), so
 * the hot loop's multiply-by-linear-gain is a bit-exact no-op at unity. */
static void
gst_dtsdec_apply_makeup_gain_db (GstDtsDec * dts, gfloat gain_db)
{
  if (gain_db > DTS_MAKEUP_GAIN_DB_MAX)
    gain_db = DTS_MAKEUP_GAIN_DB_MAX;
  else if (gain_db < DTS_MAKEUP_GAIN_DB_MIN)
    gain_db = DTS_MAKEUP_GAIN_DB_MIN;

  dts->makeup_gain_db = gain_db;
  dts->makeup_gain_linear = (gain_db == 0.0f) ? 1.0f :
      powf (10.0f, gain_db / 20.0f);
}

/* webOS 25 patch: same clamp+cache treatment for the centre-channel boost.
 * 0.0 dB yields an exact linear 1.0, so the dialogue lift is a bit-exact
 * no-op when disabled. */
static void
gst_dtsdec_apply_center_boost_db (GstDtsDec * dts, gfloat boost_db)
{
  dts->center_boost_db =
      dts_drc_clampf (boost_db, DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX);
  dts->center_boost_linear = dts_drc_db_to_linear (dts->center_boost_db);
}

/* webOS 25 patch: clear the DRC runtime state (unity gain, nothing smoothed
 * yet) and force the smoothing coefficients to be recomputed. */
static void
gst_dtsdec_drc_reset (GstDtsDec * dts)
{
  dts->drc_smoothed_db = 0.0f;
  dts->drc_prev_linear = 1.0f;
  dts->drc_attack_coef = 1.0f;
  dts->drc_release_coef = 1.0f;
  dts->drc_coef_rate = 0;
  dts->drc_coef_mode = -1;
}

/* webOS 25 patch: (re)derive the per-block one-pole coefficients for the
 * current mode and sample rate. Called only when either changes, so its two
 * expf() calls never land on the per-block path. */
static void
gst_dtsdec_drc_update_coefs (GstDtsDec * dts)
{
  gint rate = (dts->sample_rate > 0) ? dts->sample_rate : 48000;

  if (dts->drc_mode == DTS_DRC_MODE_LINE || dts->drc_mode == DTS_DRC_MODE_RF) {
    const DtsDrcProfile *p = &dts_drc_profiles[dts->drc_mode];
    gfloat period = (gfloat) DTS_DRC_BLOCK_SAMPLES / (gfloat) rate;

    dts->drc_attack_coef = dts_drc_smooth_coef (p->attack_ms, period);
    dts->drc_release_coef = dts_drc_smooth_coef (p->release_ms, period);
  } else {
    dts->drc_attack_coef = 1.0f;
    dts->drc_release_coef = 1.0f;
  }

  dts->drc_coef_rate = dts->sample_rate;
  dts->drc_coef_mode = dts->drc_mode;
}

static void
gst_dtsdec_init (GstDtsDec * dtsdec)
{
  DtsDrcConfig cfg;

  dtsdec->request_channels = DCA_CHANNEL;
  dtsdec->dynamic_range_compression = FALSE;

  /* webOS 25 patch: defaults for make-up gain, DRC and centre boost come from
   * the on-device config file (all-inert if it is absent/empty/invalid); an
   * explicit property set later (e.g. the app self-test) overrides these via
   * gst_dtsdec_set_property(). */
  dts_drc_config_read_file (DTS_MAKEUP_GAIN_CONF_PATH, &cfg);
  gst_dtsdec_apply_makeup_gain_db (dtsdec, cfg.gain_db);
  gst_dtsdec_apply_center_boost_db (dtsdec, cfg.center_db);
  dtsdec->drc_mode = cfg.drc_mode;
  dtsdec->drc_boost_pct =
      dts_drc_clampf (cfg.drc_boost_pct, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX);
  dtsdec->drc_cut_pct =
      dts_drc_clampf (cfg.drc_cut_pct, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX);
  gst_dtsdec_drc_reset (dtsdec);

  GST_INFO_OBJECT (dtsdec,
      "config at init: makeup-gain-db %.2f dB (linear %.4f), drc-mode %s, "
      "drc-boost %.0f%%, drc-cut %.0f%%, center-boost-db %.2f dB (linear %.4f)",
      dtsdec->makeup_gain_db, dtsdec->makeup_gain_linear,
      dts_drc_mode_to_string (dtsdec->drc_mode), dtsdec->drc_boost_pct,
      dtsdec->drc_cut_pct, dtsdec->center_boost_db,
      dtsdec->center_boost_linear);

  gst_audio_decoder_set_use_default_pad_acceptcaps (GST_AUDIO_DECODER_CAST
      (dtsdec), TRUE);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_AUDIO_DECODER_SINK_PAD (dtsdec));

  /* retrieve and intercept base class chain.
   * Quite HACKish, but that's dvd specs for you,
   * since one buffer needs to be split into 2 frames */
  dtsdec->base_chain = GST_PAD_CHAINFUNC (GST_AUDIO_DECODER_SINK_PAD (dtsdec));
  gst_pad_set_chain_function (GST_AUDIO_DECODER_SINK_PAD (dtsdec),
      GST_DEBUG_FUNCPTR (gst_dtsdec_chain));
}

static gboolean
gst_dtsdec_start (GstAudioDecoder * dec)
{
  GstDtsDec *dts = GST_DTSDEC (dec);
  GstDtsDecClass *klass;

  GST_DEBUG_OBJECT (dec, "start");

  klass = GST_DTSDEC_CLASS (G_OBJECT_GET_CLASS (dts));
  dts->state = dca_init (klass->dts_cpuflags);
  dts->samples = dca_samples (dts->state);
  dts->bit_rate = -1;
  dts->sample_rate = -1;
  dts->stream_channels = DCA_CHANNEL;
  dts->using_channels = DCA_CHANNEL;
  dts->level = 1;
  dts->bias = 0;
  dts->flag_update = TRUE;

  /* webOS 25 patch: a new stream starts with the compressor at unity. */
  gst_dtsdec_drc_reset (dts);

  /* call upon legacy upstream byte support (e.g. seeking) */
  gst_audio_decoder_set_estimate_rate (dec, TRUE);

  return TRUE;
}

static gboolean
gst_dtsdec_stop (GstAudioDecoder * dec)
{
  GstDtsDec *dts = GST_DTSDEC (dec);

  GST_DEBUG_OBJECT (dec, "stop");

  dts->samples = NULL;
  if (dts->state) {
    dca_free (dts->state);
    dts->state = NULL;
  }

  return TRUE;
}

static GstFlowReturn
gst_dtsdec_parse (GstAudioDecoder * bdec, GstAdapter * adapter,
    gint * _offset, gint * len)
{
  GstDtsDec *dts;
  guint8 *data;
  gint av, size;
  gint length = 0, flags, sample_rate, bit_rate, frame_length;
  GstFlowReturn result = GST_FLOW_EOS;

  dts = GST_DTSDEC (bdec);

  size = av = gst_adapter_available (adapter);
  data = (guint8 *) gst_adapter_map (adapter, av);

  /* find and read header */
  bit_rate = dts->bit_rate;
  sample_rate = dts->sample_rate;
  flags = 0;
  while (size >= 7) {
    length = dca_syncinfo (dts->state, data, &flags,
        &sample_rate, &bit_rate, &frame_length);

    if (length == 0) {
      /* shift window to re-find sync */
      data++;
      size--;
    } else if (length <= size) {
      GST_LOG_OBJECT (dts, "Sync: frame size %d", length);
      result = GST_FLOW_OK;
      break;
    } else {
      GST_LOG_OBJECT (dts, "Not enough data available (needed %d had %d)",
          length, size);
      break;
    }
  }
  gst_adapter_unmap (adapter);

  *_offset = av - size;
  *len = length;

  return result;
}

static gint
gst_dtsdec_channels (uint32_t flags, GstAudioChannelPosition * pos)
{
  gint chans = 0;

  switch (flags & DCA_CHANNEL_MASK) {
    case DCA_MONO:
      chans = 1;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
      }
      break;
      /* case DCA_CHANNEL: */
    case DCA_STEREO:
    case DCA_STEREO_SUMDIFF:
    case DCA_STEREO_TOTAL:
    case DCA_DOLBY:
      chans = 2;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      }
      break;
    case DCA_3F:
      chans = 3;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
      }
      break;
    case DCA_2F1R:
      chans = 3;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
      }
      break;
    case DCA_3F1R:
      chans = 4;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
      }
      break;
    case DCA_2F2R:
      chans = 4;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    case DCA_3F2R:
      chans = 5;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[4] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    case DCA_4F2R:
      chans = 6;
      if (pos) {
        pos[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
        pos[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
        pos[2] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        pos[3] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
        pos[4] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
        pos[5] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
      }
      break;
    default:
      g_warning ("dtsdec: invalid flags 0x%x", flags);
      return 0;
  }
  if (flags & DCA_LFE) {
    if (pos) {
      pos[chans] = GST_AUDIO_CHANNEL_POSITION_LFE1;
    }
    chans += 1;
  }

  return chans;
}

static gboolean
gst_dtsdec_renegotiate (GstDtsDec * dts)
{
  gint channels;
  gboolean result = FALSE;
  GstAudioChannelPosition from[7], to[7];
  GstAudioInfo info;

  channels = gst_dtsdec_channels (dts->using_channels, from);

  if (channels <= 0 || channels > 7)
    goto done;

  GST_INFO_OBJECT (dts, "dtsdec renegotiate, channels=%d, rate=%d",
      channels, dts->sample_rate);

  memcpy (to, from, sizeof (GstAudioChannelPosition) * channels);
  gst_audio_channel_positions_to_valid_order (to, channels);
  gst_audio_get_channel_reorder_map (channels, from, to,
      dts->channel_reorder_map);


  gst_audio_info_init (&info);
  gst_audio_info_set_format (&info,
      SAMPLE_TYPE, dts->sample_rate, channels, (channels > 1 ? to : NULL));

  if (!gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (dts), &info))
    goto done;

  result = TRUE;

done:
  return result;
}

static void
gst_dtsdec_update_streaminfo (GstDtsDec * dts)
{
  GstTagList *taglist;

  if (dts->bit_rate > 3) {
    taglist = gst_tag_list_new_empty ();
    /* 1 => open bitrate, 2 => variable bitrate, 3 => lossless */
    gst_tag_list_add (taglist, GST_TAG_MERGE_APPEND, GST_TAG_BITRATE,
        (guint) dts->bit_rate, NULL);
    gst_audio_decoder_merge_tags (GST_AUDIO_DECODER (dts), taglist,
        GST_TAG_MERGE_REPLACE);
    if (taglist)
      gst_tag_list_unref (taglist);
  }
}

static GstFlowReturn
gst_dtsdec_handle_frame (GstAudioDecoder * bdec, GstBuffer * buffer)
{
  GstDtsDec *dts;
  gint channels, i, num_blocks;
  gboolean need_renegotiation = FALSE;
  guint8 *data;
  GstMapInfo map;
  gint chans;
#ifndef G_DISABLE_ASSERT
  gsize size;
  gint length;
#endif
  gint flags, sample_rate, bit_rate, frame_length;
  GstFlowReturn result = GST_FLOW_OK;
  GstBuffer *outbuf;
  /* webOS 25 patch: DRC/centre-boost per-frame setup. */
  GstAudioChannelPosition chan_pos[7];
  gint center_idx = -1, lfe_idx = -1, ci;
  gboolean gain_only;

  dts = GST_DTSDEC (bdec);

  /* no fancy draining */
  if (G_UNLIKELY (!buffer))
    return GST_FLOW_OK;

  /* parsed stuff already, so this should work out fine */
  gst_buffer_map (buffer, &map, GST_MAP_READ);
  data = map.data;

#ifndef G_DISABLE_ASSERT
  size = map.size;
  g_assert (size >= 7);
#endif

  bit_rate = dts->bit_rate;
  sample_rate = dts->sample_rate;
  flags = 0;

#ifndef G_DISABLE_ASSERT
  length = dca_syncinfo (dts->state, data, &flags, &sample_rate, &bit_rate,
      &frame_length);
  g_assert (length == size);
#else
  (void) dca_syncinfo (dts->state, data, &flags, &sample_rate, &bit_rate,
      &frame_length);
#endif

  if (flags != dts->prev_flags) {
    dts->prev_flags = flags;
    dts->flag_update = TRUE;
  }

  /* go over stream properties, renegotiate or update streaminfo if needed */
  if (dts->sample_rate != sample_rate) {
    need_renegotiation = TRUE;
    dts->sample_rate = sample_rate;
  }

  if (flags) {
    dts->stream_channels = flags & (DCA_CHANNEL_MASK | DCA_LFE);
  }

  if (bit_rate != dts->bit_rate) {
    dts->bit_rate = bit_rate;
    gst_dtsdec_update_streaminfo (dts);
  }

  /* If we haven't had an explicit number of channels chosen through properties
   * at this point, choose what to downmix to now, based on what the peer will
   * accept - this allows a52dec to do downmixing in preference to a
   * downstream element such as audioconvert.
   * FIXME: Add the property back in for forcing output channels.
   */
  if (dts->request_channels != DCA_CHANNEL) {
    flags = dts->request_channels;
  } else if (dts->flag_update) {
    GstCaps *caps;

    dts->flag_update = FALSE;

    caps = gst_pad_get_allowed_caps (GST_AUDIO_DECODER_SRC_PAD (dts));
    if (caps && gst_caps_get_size (caps) > 0) {
      GstCaps *copy = gst_caps_copy_nth (caps, 0);
      GstStructure *structure = gst_caps_get_structure (copy, 0);
      gint channels;
      const int dts_channels[6] = {
        DCA_MONO,
        DCA_STEREO,
        DCA_STEREO | DCA_LFE,
        DCA_2F2R,
        DCA_2F2R | DCA_LFE,
        DCA_3F2R | DCA_LFE,
      };

      /* Prefer the original number of channels, but fixate to something
       * preferred (first in the caps) downstream if possible.
       */
      gst_structure_fixate_field_nearest_int (structure, "channels",
          flags ? gst_dtsdec_channels (flags, NULL) : 6);
      gst_structure_get_int (structure, "channels", &channels);
      if (channels <= 6)
        flags = dts_channels[channels - 1];
      else
        flags = dts_channels[5];

      gst_caps_unref (copy);
    } else if (flags) {
      flags = dts->stream_channels;
    } else {
      flags = DCA_3F2R | DCA_LFE;
    }

    if (caps)
      gst_caps_unref (caps);
  } else {
    flags = dts->using_channels;
  }

  /* process */
  flags |= DCA_ADJUST_LEVEL;
  dts->level = 1;
  if (dca_frame (dts->state, data, &flags, &dts->level, dts->bias)) {
    gst_buffer_unmap (buffer, &map);
    GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
        ("dts_frame error"), result);
    goto exit;
  }
  gst_buffer_unmap (buffer, &map);

  channels = flags & (DCA_CHANNEL_MASK | DCA_LFE);
  if (dts->using_channels != channels) {
    need_renegotiation = TRUE;
    dts->using_channels = channels;
  }

  /* negotiate if required */
  if (need_renegotiation) {
    GST_DEBUG_OBJECT (dts,
        "dtsdec: sample_rate:%d stream_chans:0x%x using_chans:0x%x",
        dts->sample_rate, dts->stream_channels, dts->using_channels);
    if (!gst_dtsdec_renegotiate (dts))
      goto failed_negotiation;
  }

  if (dts->dynamic_range_compression == FALSE) {
    dca_dynrng (dts->state, NULL, NULL);
  }

  flags &= (DCA_CHANNEL_MASK | DCA_LFE);
  chans = gst_dtsdec_channels (flags, chan_pos);
  if (!chans)
    goto invalid_flags;

  /* webOS 25 patch: locate the front-centre channel (the dialogue lift target)
   * and the LFE (excluded from the DRC level detector) in libdca's planar
   * output order — chan_pos[i] describes dts->samples[i * 256 ...]. Layouts
   * without a discrete centre (mono, stereo, 2F2R) simply get no lift. */
  for (ci = 0; ci < chans; ci++) {
    if (chan_pos[ci] == GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER)
      center_idx = ci;
    else if (chan_pos[ci] == GST_AUDIO_CHANNEL_POSITION_LFE1)
      lfe_idx = ci;
  }

  /* webOS 25 patch: with no DRC and no centre boost there is nothing to do
   * beyond the make-up gain, so we run the previously shipped output loop
   * verbatim. That keeps output bit-identical to the shipped build for ANY
   * make-up gain value, and at 0.0 dB the multiply is by an exact 1.0f — i.e.
   * the epic's "drc=off, center=0, gain=0 is a bit-exact no-op" guarantee,
   * with no added multiply on that path. */
  gain_only = (dts->drc_mode == DTS_DRC_MODE_OFF
      && dts->center_boost_db == 0.0f);

  if (!gain_only && (dts->drc_coef_rate != dts->sample_rate
          || dts->drc_coef_mode != dts->drc_mode))
    gst_dtsdec_drc_update_coefs (dts);

  /* handle decoded data, one block is 256 samples */
  num_blocks = dca_blocks_num (dts->state);
  outbuf =
      gst_buffer_new_and_alloc (256 * chans * (SAMPLE_WIDTH / 8) * num_blocks);

  gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
  data = map.data;
  {
    guint8 *ptr = data;
    for (i = 0; i < num_blocks; i++) {
      if (dca_block (dts->state)) {
        /* also marks discont */
        GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
            ("error decoding block %d", i), result);
        if (result != GST_FLOW_OK)
          goto exit;
      } else if (gain_only) {
        gint n, c;
        gint *reorder_map = dts->channel_reorder_map;

        for (n = 0; n < 256; n++) {
          for (c = 0; c < chans; c++) {
            {
              /* webOS: convert libdca's normalized float (~[-1,1]) to S32LE
               * with clamping, so LG's integer-only audiosink accepts it.
               * webOS 25 patch: apply the user-tunable make-up gain (linear;
               * default 1.0 = exact no-op) BEFORE the scale/clamp below, so
               * the existing clipping guard still protects the output. */
              gdouble sample = (gdouble) dts->samples[c * 256 + n] *
                  (gdouble) dts->makeup_gain_linear;
              gdouble s = sample * 2147483648.0;
              if (s > 2147483647.0)
                s = 2147483647.0;
              else if (s < -2147483648.0)
                s = -2147483648.0;
              ((gint32 *) ptr)[n * chans + reorder_map[c]] = (gint32) s;
            }
          }
        }
      } else {
        /* webOS 25 patch: DRC and/or centre-boost path. See the DRC CORE
         * block for the algorithm; this is only its per-block host binding. */
        gint n, c;
        gint *reorder_map = dts->channel_reorder_map;
        gfloat drc_linear = 1.0f;
        gfloat from, to;

        if (dts->drc_mode != DTS_DRC_MODE_OFF) {
          gint count = 0;
          gfloat sum_sq, level_db, target_db;

          /* detector: block RMS over the full-range channels only */
          sum_sq = dts_drc_sum_squares ((const gfloat *) dts->samples, chans,
              256, lfe_idx, &count);
          level_db = dts_drc_level_dbfs (sum_sq, count);
          target_db = dts_drc_scale_gain_db (dts_drc_target_gain_db
              (dts->drc_mode, level_db), dts->drc_boost_pct,
              dts->drc_cut_pct);
          dts->drc_smoothed_db = dts_drc_smooth_step (dts->drc_smoothed_db,
              target_db, dts->drc_attack_coef, dts->drc_release_coef);
          drc_linear = dts_drc_db_to_linear (dts->drc_smoothed_db);
        } else {
          dts->drc_smoothed_db = 0.0f;
        }

        /* Fold the make-up gain into the block's start/end factors, then ramp
         * linearly from the previous block's value so gain changes cannot
         * zipper. The DRC factor is identical on every channel (the stereo /
         * surround image is preserved); only the centre gets the extra lift. */
        from = dts->drc_prev_linear * dts->makeup_gain_linear;
        to = drc_linear * dts->makeup_gain_linear;
        dts->drc_prev_linear = drc_linear;

        /* BEGIN DRC per-sample apply — must stay free of transcendentals and
         * of doubles in the gain arithmetic (armel soft-float target): 1 float
         * add + 1 float multiply per sample. The double scale/clamp below is
         * the pre-existing S32 conversion, kept unchanged. The marker starts
         * HERE, above the gain declarations, so run-tests.sh actually sees
         * them — inside the inner loop alone the check would be vacuous. */
        for (c = 0; c < chans; c++) {
          gfloat chan_scale =
              (c == center_idx) ? dts->center_boost_linear : 1.0f;
          gfloat g = from * chan_scale;
          gfloat step = (to - from) * chan_scale * (1.0f / 256.0f);
          const sample_t *in = dts->samples + c * 256;
          gint32 *out = ((gint32 *) ptr) + reorder_map[c];

          for (n = 0; n < 256; n++) {
            gdouble s;

            g += step;
            s = (gdouble) ((gfloat) in[n] * g) * 2147483648.0;
            if (s > 2147483647.0)
              s = 2147483647.0;
            else if (s < -2147483648.0)
              s = -2147483648.0;
            out[n * chans] = (gint32) s;
          }
        }
        /* END DRC per-sample apply */
      }
      ptr += 256 * chans * (SAMPLE_WIDTH / 8);
    }
  }
  gst_buffer_unmap (outbuf, &map);

  result = gst_audio_decoder_finish_frame (bdec, outbuf, 1);

exit:
  return result;

  /* ERRORS */
failed_negotiation:
  {
    GST_ELEMENT_ERROR (dts, CORE, NEGOTIATION, (NULL), (NULL));
    return GST_FLOW_ERROR;
  }
invalid_flags:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Invalid channel flags: %d", flags));
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_dtsdec_set_format (GstAudioDecoder * bdec, GstCaps * caps)
{
  GstDtsDec *dts = GST_DTSDEC (bdec);
  GstStructure *structure;

  structure = gst_caps_get_structure (caps, 0);

  if (structure && gst_structure_has_name (structure, "audio/x-private1-dts"))
    dts->dvdmode = TRUE;
  else
    dts->dvdmode = FALSE;

  return TRUE;
}

static GstFlowReturn
gst_dtsdec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstDtsDec *dts = GST_DTSDEC (parent);
  gint first_access;

  if (dts->dvdmode) {
    guint8 data[2];
    gsize size;
    gint offset, len;
    GstBuffer *subbuf;

    size = gst_buffer_get_size (buf);
    if (size < 2)
      goto not_enough_data;

    gst_buffer_extract (buf, 0, data, 2);
    first_access = (data[0] << 8) | data[1];

    /* Skip the first_access header */
    offset = 2;

    if (first_access > 1) {
      /* Length of data before first_access */
      len = first_access - 1;

      if (len <= 0 || offset + len > size)
        goto bad_first_access_parameter;

      subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
      GST_BUFFER_TIMESTAMP (subbuf) = GST_CLOCK_TIME_NONE;
      ret = dts->base_chain (pad, parent, subbuf);
      if (ret != GST_FLOW_OK) {
        gst_buffer_unref (buf);
        goto done;
      }

      offset += len;
      len = size - offset;

      if (len > 0) {
        subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
        GST_BUFFER_TIMESTAMP (subbuf) = GST_BUFFER_TIMESTAMP (buf);

        ret = dts->base_chain (pad, parent, subbuf);
      }
      gst_buffer_unref (buf);
    } else {
      /* first_access = 0 or 1, so if there's a timestamp it applies to the first byte */
      subbuf =
          gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset,
          size - offset);
      GST_BUFFER_TIMESTAMP (subbuf) = GST_BUFFER_TIMESTAMP (buf);
      ret = dts->base_chain (pad, parent, subbuf);
      gst_buffer_unref (buf);
    }
  } else {
    ret = dts->base_chain (pad, parent, buf);
  }

done:
  return ret;

/* ERRORS */
not_enough_data:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Insufficient data in buffer. Can't determine first_acess"));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
bad_first_access_parameter:
  {
    GST_ELEMENT_ERROR (GST_ELEMENT (dts), STREAM, DECODE, (NULL),
        ("Bad first_access parameter (%d) in buffer", first_access));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
}

static void
gst_dtsdec_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDEC (object);

  switch (prop_id) {
    case PROP_DRC:
      dts->dynamic_range_compression = g_value_get_boolean (value);
      break;
    case PROP_MAKEUP_GAIN_DB:
      gst_dtsdec_apply_makeup_gain_db (dts, g_value_get_float (value));
      break;
    case PROP_DRC_MODE:
      dts->drc_mode = dts_drc_mode_from_string (g_value_get_string (value));
      break;
    case PROP_DRC_BOOST:
      dts->drc_boost_pct = dts_drc_clampf (g_value_get_float (value),
          DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX);
      break;
    case PROP_DRC_CUT:
      dts->drc_cut_pct = dts_drc_clampf (g_value_get_float (value),
          DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX);
      break;
    case PROP_CENTER_BOOST_DB:
      gst_dtsdec_apply_center_boost_db (dts, g_value_get_float (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsdec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDEC (object);

  switch (prop_id) {
    case PROP_DRC:
      g_value_set_boolean (value, dts->dynamic_range_compression);
      break;
    case PROP_MAKEUP_GAIN_DB:
      g_value_set_float (value, dts->makeup_gain_db);
      break;
    case PROP_DRC_MODE:
      g_value_set_static_string (value,
          dts_drc_mode_to_string (dts->drc_mode));
      break;
    case PROP_DRC_BOOST:
      g_value_set_float (value, dts->drc_boost_pct);
      break;
    case PROP_DRC_CUT:
      g_value_set_float (value, dts->drc_cut_pct);
      break;
    case PROP_CENTER_BOOST_DB:
      g_value_set_float (value, dts->center_boost_db);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
dtsdec_element_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (dtsdec_debug, "dtsdec", 0, "DTS/DCA audio decoder");

#if HAVE_ORC
  orc_init ();
#endif

  return gst_element_register (plugin, "dtsdec", GST_RANK_PRIMARY,
      GST_TYPE_DTSDEC);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (dtsdec, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dtsdec,
    "Decodes DTS audio streams",
    plugin_init, VERSION, "GPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
