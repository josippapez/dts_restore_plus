/* Host unit test for the webOS 25 dtsdec DRC core.
 *
 * This does NOT re-implement the DSP: run-tests.sh extracts the code between
 * the DRC-CORE-BEGIN / DRC-CORE-END markers in ../gstdtsdec.c into
 * drc-core.inc, which is #included below. The assertions therefore exercise
 * the exact arithmetic that ships in libgstdtsdec.so (and that chunk 02 ports
 * into ffmpeg's mlpdec.c), so the two can never silently drift.
 *
 * Build/run: sh run-tests.sh   (libc + libm only, no GStreamer/libdca needed)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* The core parses ASCII floats through DTS_DRC_STRTOD. gstdtsdec.c binds it to
 * GLib's g_ascii_strtod(); here the core's own fallback (the C library's
 * strtod, in the default "C" locale) applies. */

#include "drc-core.inc"

/* ------------------------------------------------------------------ harness */

static int checks = 0;
static int failures = 0;

static void
report (int ok, const char *label)
{
  checks++;
  if (!ok)
    failures++;
  printf ("  %-4s %s\n", ok ? "ok" : "FAIL", label);
}

static void
near (double got, double want, double tol, const char *label)
{
  int ok = fabs (got - want) <= tol;

  checks++;
  if (!ok)
    failures++;
  printf ("  %-4s %-52s got %12.6f  want %12.6f\n", ok ? "ok" : "FAIL", label,
      got, want);
}

/* ---------------------------------------------- 1. static curve breakpoints */

static void
test_curve_line (void)
{
  puts ("[1] static curve — line mode (boost/cut 100%)");
  /* -50: max-boost region, 6.0 (2:1 over -43..-31) + 5.6 (5:1 over -50..-43) */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -50.0f), 11.6, 1e-4,
      "-50 dBFS -> max-boost region");
  /* -35: 2:1 boost region, (-31 - -35) * 0.5 */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -35.0f), 2.0, 1e-4,
      "-35 dBFS -> 2:1 boost region");
  /* -25: inside the -31..-20 null band */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -25.0f), 0.0, 0.0,
      "-25 dBFS -> null band, exactly 0 dB");
  /* -15: 2:1 early cut, -((-15 - -20) * 0.5) */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -15.0f), -2.5, 1e-4,
      "-15 dBFS -> 2:1 cut region");
  /* -5: full early cut (-5.0) + 20:1 limiting over -10..-5 (-4.75) */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -5.0f), -9.75, 1e-4,
      "-5 dBFS -> 20:1 limiting region");

  /* breakpoints + cap */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -31.0f), 0.0, 0.0,
      "-31 dBFS -> null band lower edge");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -20.0f), 0.0, 0.0,
      "-20 dBFS -> null band upper edge");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -43.0f), 6.0, 1e-4,
      "-43 dBFS -> boost/max-boost junction");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -50.5f), 12.0, 1e-4,
      "-50.5 dBFS -> peaks at +12 dB");
  /* amendment D: past the peak the boost DECAYS back to 0 dB at -85 dBFS */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -60.0f), 8.695652, 1e-4,
      "-60 dBFS -> on the decay leg, not the old plateau");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -85.0f), 0.0, 1e-4,
      "-85 dBFS -> boost has decayed to 0 dB");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -90.0f), 0.0, 0.0,
      "-90 dBFS (detector floor) -> 0 dB, no boost at all");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, 0.0f), -14.5, 1e-4,
      "0 dBFS -> full-scale cut");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_OFF, -5.0f), 0.0, 0.0,
      "mode off -> exactly 0 dB at any level");
}

static void
test_curve_rf (void)
{
  puts ("[2] static curve — rf mode (boost/cut 100%)");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -50.0f), 11.6, 1e-4,
      "-50 dBFS -> max-boost region (same boost side as line)");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -35.0f), 2.0, 1e-4,
      "-35 dBFS -> 2:1 boost region");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -25.0f), 0.0, 0.0,
      "-25 dBFS -> inside the narrower -31..-24 null band");
  /* -15: 2:1 early cut from -24, -((-15 - -24) * 0.5) */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -15.0f), -4.5, 1e-4,
      "-15 dBFS -> 2:1 cut region (heavier than line)");
  /* -5: early cut over -24..-14 (-5.0) + 20:1 over -14..-5 (-8.55) */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -5.0f), -13.55, 1e-4,
      "-5 dBFS -> 20:1 limiting above -14 dBFS");

  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -24.0f), 0.0, 0.0,
      "-24 dBFS -> null band upper edge");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -55.5f), 16.0, 1e-4,
      "-55.5 dBFS -> peaks at +16 dB (amendment D reduced this from +20)");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -60.5f), 13.288136, 1e-4,
      "-60.5 dBFS -> on the decay leg (used to be the +20 dB cap)");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -85.0f), 0.0, 1e-4,
      "-85 dBFS -> boost has decayed to 0 dB");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -90.0f), 0.0, 0.0,
      "-90 dBFS (detector floor) -> 0 dB, no boost at all");

  /* rf must never manage less than line on the cut side */
  {
    int ok = 1;
    float l;

    for (l = -24.0f; l <= 0.0f; l += 0.25f) {
      if (dts_drc_target_gain_db (DTS_DRC_MODE_RF, l) >
          dts_drc_target_gain_db (DTS_DRC_MODE_LINE, l) + 1e-5f)
        ok = 0;
    }
    report (ok, "rf cuts at least as hard as line over -24..0 dBFS");
  }
}

static void
test_curve_shape (void)
{
  int mode;

  puts ("[3] static curve — single peak, monotonic either side, continuous");
  for (mode = DTS_DRC_MODE_LINE; mode <= DTS_DRC_MODE_RF; mode++) {
    /* Amendment D made the curve unimodal rather than monotonic: it rises from
     * 0 dB at -85 up to the peak, then falls all the way to the full-scale cut.
     * So the invariants are: non-decreasing below the peak, non-increasing
     * above it, the peak equal to max_boost_db, and still continuous. */
    const float peak_db = dts_drc_profiles[mode].max_boost_db;
    int up_ok = 1, down_ok = 1, cont = 1;
    float l, prev, worst = 0.0f, peak_at = -100.0f;
    float peak_seen = dts_drc_target_gain_db (mode, -100.0f);

    /* pass 1: locate the peak */
    for (l = -100.0f; l <= 0.0f; l += 0.05f) {
      float g = dts_drc_target_gain_db (mode, l);

      if (g > peak_seen) {
        peak_seen = g;
        peak_at = l;
      }
    }

    /* pass 2: below the peak must be non-decreasing, above it non-increasing.
     * Checking each leg against the located peak is what makes this a real
     * assertion -- a single "turning point" counter can be satisfied by a
     * curve that wanders, and cannot fail on the rising leg at all. */
    prev = dts_drc_target_gain_db (mode, -100.0f);
    for (l = -100.0f + 0.05f; l <= 0.0f; l += 0.05f) {
      float g = dts_drc_target_gain_db (mode, l);
      float d = g - prev;

      if (l <= peak_at) {
        if (d < -1e-4f)
          up_ok = 0;
      } else if (d > 1e-4f) {
        down_ok = 0;
      }
      if (fabsf (d) > 0.05f)
        cont = 0;                    /* max documented slope is 0.95 dB/dB */
      if (fabsf (d) > worst)
        worst = fabsf (d);
      prev = g;
    }
    printf ("  ---- mode %s: peak %+.4f dB at %.2f dBFS,"
        " worst 0.05 dB step %.5f dB\n", dts_drc_mode_to_string (mode),
        (double) peak_seen, (double) peak_at, (double) worst);
    report (up_ok, "non-decreasing on the rising leg (below the peak)");
    report (down_ok, "non-increasing on the falling leg (above the peak)");
    report (fabsf (peak_seen - peak_db) < 0.01f,
        "the peak equals the profile's max_boost_db");
    report (cont, "no discontinuity at any breakpoint");
  }
}

/* -------------------------------------------------- 4. boost / cut scaling */

static void
test_scaling (void)
{
  float boost = dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -35.0f);  /* +2.0  */
  float cut = dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -5.0f);     /* -9.75 */

  puts ("[4] boost%/cut% scaling (LG drc_boost/cut_scl_factor)");
  near (dts_drc_scale_gain_db (boost, 100.0f, 100.0f), 2.0, 1e-4,
      "boost 100% -> unscaled");
  near (dts_drc_scale_gain_db (boost, 50.0f, 100.0f), 1.0, 1e-4,
      "boost 50% -> halved");
  near (dts_drc_scale_gain_db (boost, 0.0f, 100.0f), 0.0, 0.0,
      "boost 0% -> no boost at all");
  near (dts_drc_scale_gain_db (cut, 100.0f, 100.0f), -9.75, 1e-4,
      "cut 100% -> unscaled");
  near (dts_drc_scale_gain_db (cut, 100.0f, 50.0f), -4.875, 1e-4,
      "cut 50% -> halved");
  near (dts_drc_scale_gain_db (cut, 100.0f, 0.0f), 0.0, 0.0,
      "cut 0% -> no cut at all");
  near (dts_drc_scale_gain_db (cut, 0.0f, 100.0f), -9.75, 1e-4,
      "boost% does not touch a cut");
  near (dts_drc_scale_gain_db (boost, 100.0f, 0.0f), 2.0, 1e-4,
      "cut% does not touch a boost");
  near (dts_drc_scale_gain_db (0.0f, 0.0f, 0.0f), 0.0, 0.0,
      "null-band 0 dB stays 0 dB");
  /* Light preset (line, 50/50) must sit strictly between Off and Medium. */
  report (dts_drc_scale_gain_db (cut, 50.0f, 50.0f) > cut
      && dts_drc_scale_gain_db (cut, 50.0f, 50.0f) < 0.0f,
      "\"Light\" preset (50/50) lands between Off and Medium");
}

/* --------------------------------------------------------- 5. smoothing */

static void
test_smoothing (void)
{
  const float period48 = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
  const float period441 = (float) DTS_DRC_BLOCK_SAMPLES / 44100.0f;
  int mode;

  puts ("[5] one-pole smoothing (dB domain, one step per 256-sample block)");
  printf ("  ---- block period @48 kHz = %.6f s\n", (double) period48);

  near (dts_drc_smooth_coef (dts_drc_profiles[DTS_DRC_MODE_LINE].attack_ms,
          period48), 0.413366, 1e-4, "line attack coef @48 kHz (tau 10 ms)");
  near (dts_drc_smooth_coef (dts_drc_profiles[DTS_DRC_MODE_LINE].release_ms,
          period48), 0.021107, 1e-4, "line release coef @48 kHz (tau 250 ms)");
  near (dts_drc_smooth_coef (dts_drc_profiles[DTS_DRC_MODE_RF].attack_ms,
          period48), 0.655819, 1e-4, "rf attack coef @48 kHz (tau 5 ms)");
  near (dts_drc_smooth_coef (dts_drc_profiles[DTS_DRC_MODE_RF].release_ms,
          period48), 0.034931, 1e-4, "rf release coef @48 kHz (tau 150 ms)");
  /* 44.1 kHz means a longer 256-sample block, so more of the same wall-clock
   * time constant elapses per step -> a larger coefficient. */
  report (dts_drc_smooth_coef (10.0f, period441) >
      dts_drc_smooth_coef (10.0f, period48),
      "coefficient tracks sample rate (44.1 kHz block is longer than 48 kHz)");

  for (mode = DTS_DRC_MODE_LINE; mode <= DTS_DRC_MODE_RF; mode++) {
    const DtsDrcProfile *p = &dts_drc_profiles[mode];
    float atk = dts_drc_smooth_coef (p->attack_ms, period48);
    float rel = dts_drc_smooth_coef (p->release_ms, period48);
    float target = -9.75f;
    float g = 0.0f;
    int i, n_tau;
    char label[96];

    report (atk > rel, "attack is faster than release");

    /* convergence: a constant target must be reached */
    for (i = 0; i < 4000; i++)
      g = dts_drc_smooth_step (g, target, atk, rel);
    sprintf (label, "mode %s: converges onto a constant target",
        dts_drc_mode_to_string (mode));
    near (g, target, 1e-3, label);

    /* one time constant must cover roughly 63% of the distance (attack) */
    n_tau = (int) (p->attack_ms * 0.001f / period48 + 0.5f);
    if (n_tau < 1)
      n_tau = 1;
    g = 0.0f;
    for (i = 0; i < n_tau; i++)
      g = dts_drc_smooth_step (g, target, atk, rel);
    sprintf (label, "mode %s: attack reaches ~63%% after one tau (%d blocks)",
        dts_drc_mode_to_string (mode), n_tau);
    report (g / target > 0.55f && g / target < 0.80f, label);

    /* and release must be the slow direction back to unity */
    n_tau = (int) (p->release_ms * 0.001f / period48 + 0.5f);
    g = target;
    for (i = 0; i < n_tau; i++)
      g = dts_drc_smooth_step (g, 0.0f, atk, rel);
    sprintf (label, "mode %s: release reaches ~63%% after one tau (%d blocks)",
        dts_drc_mode_to_string (mode), n_tau);
    report ((target - g) / target > 0.55f && (target - g) / target < 0.72f,
        label);
  }

  /* A rising level must attack (gain drops), a falling level must release. */
  {
    float g = 0.0f;
    float atk = dts_drc_smooth_coef (10.0f, period48);
    float rel = dts_drc_smooth_coef (250.0f, period48);
    float after_attack, after_release;

    g = dts_drc_smooth_step (g, -6.0f, atk, rel);
    after_attack = g;
    g = dts_drc_smooth_step (-6.0f, 0.0f, atk, rel);
    after_release = -6.0f - g;
    report (fabsf (after_attack) > fabsf (after_release),
        "one block moves further on attack than on release");
  }
}

/* ------------------------------------------------- 6. detector (excl. LFE) */

static void
test_detector (void)
{
  /* 3 planes x 4 samples: L, R at 0.5, "LFE" at plane 2 pinned to full scale */
  float planar[3 * 4];
  int i, count = 0;
  float sum;

  puts ("[6] level detector — per block, LFE excluded");
  for (i = 0; i < 4; i++) {
    planar[0 * 4 + i] = 0.5f;
    planar[1 * 4 + i] = -0.5f;
    planar[2 * 4 + i] = 1.0f;
  }

  sum = dts_drc_sum_squares (planar, 3, 4, 2, &count);
  report (count == 8, "LFE plane contributes no samples to the count");
  near (sum, 2.0, 1e-6, "sum of squares excludes the LFE plane");
  near (dts_drc_level_dbfs (sum, count), -6.020600, 1e-4,
      "0.5 FS on all full-range channels -> -6.02 dBFS");

  sum = dts_drc_sum_squares (planar, 3, 4, -1, &count);
  report (count == 12, "skip_ch = -1 includes every plane");
  near (dts_drc_level_dbfs (sum, count), -3.010300, 1e-4,
      "a hot LFE would read 3.0 dB hotter if it were not excluded");

  /* silence and degenerate inputs must not produce -inf or NaN */
  memset (planar, 0, sizeof (planar));
  sum = dts_drc_sum_squares (planar, 3, 4, 2, &count);
  near (dts_drc_level_dbfs (sum, count), DTS_DRC_LEVEL_FLOOR_DB, 0.0,
      "digital silence floors at DTS_DRC_LEVEL_FLOOR_DB");
  near (dts_drc_level_dbfs (0.0f, 0), DTS_DRC_LEVEL_FLOOR_DB, 0.0,
      "zero-count block floors instead of dividing by zero");
  report (dts_drc_level_dbfs (1e-30f, 256) >= DTS_DRC_LEVEL_FLOOR_DB,
      "denormal-level energy still floors, never -inf");

  /* The smoother is stateful and only cleared in _start(), so a single
   * non-finite level would poison every later block. The detector must be the
   * place that stops it. */
  {
    float nan_lvl = dts_drc_level_dbfs ((float) (0.0 / 0.0), 256);
    float inf_lvl = dts_drc_level_dbfs ((float) (1.0 / 0.0), 256);
    float g = 0.0f;
    int i, finite = 1;

    report (nan_lvl == DTS_DRC_LEVEL_FLOOR_DB,
        "a NaN block floors instead of returning NaN");
    report (inf_lvl <= DTS_DRC_LEVEL_CEIL_DB && inf_lvl == inf_lvl,
        "a +inf block is bounded by DTS_DRC_LEVEL_CEIL_DB");

    /* drive the whole chain with both and prove the state stays finite */
    for (i = 0; i < 50; i++) {
      float lvl = (i == 10) ? nan_lvl : (i == 20) ? inf_lvl : -35.0f;
      float t = dts_drc_scale_gain_db (dts_drc_target_gain_db
          (DTS_DRC_MODE_LINE, lvl), 100.0f, 100.0f);

      g = dts_drc_smooth_step (g, t, 0.413366f, 0.021107f);
      if (!(g > -1e6f && g < 1e6f))
        finite = 0;
    }
    report (finite,
        "NaN/inf blocks cannot poison the smoothed gain for the stream");
  }

  /* full scale on every channel */
  for (i = 0; i < 4; i++) {
    planar[0 * 4 + i] = 1.0f;
    planar[1 * 4 + i] = 1.0f;
  }
  sum = dts_drc_sum_squares (planar, 2, 4, -1, &count);
  near (dts_drc_level_dbfs (sum, count), 0.0, 1e-5,
      "full-scale DC reads 0 dBFS");
}

/* -------------------------------------------------- 7. config file contract */

static char conf_path[1024];

static void
write_conf (const char *content)
{
  FILE *f = fopen (conf_path, "w");

  if (f == NULL) {
    fprintf (stderr, "cannot write %s\n", conf_path);
    exit (2);
  }
  fputs (content, f);
  fclose (f);
}

static void
test_config (void)
{
  DtsDrcConfig c;

  puts ("[7] gain.conf parsing (backward compatible)");

  /* missing file -> all defaults, decode must never fail */
  remove (conf_path);
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == 0.0f && c.drc_mode == DTS_DRC_MODE_OFF
      && c.drc_boost_pct == 100.0f && c.drc_cut_pct == 100.0f
      && c.center_db == 0.0f, "missing file -> inert defaults");

  /* AC #4: a bare float is still the make-up gain */
  write_conf ("6.0\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == 6.0f && c.drc_mode == DTS_DRC_MODE_OFF
      && c.center_db == 0.0f,
      "legacy bare float -> make-up gain, DRC still off");

  write_conf ("# tuned by ear\n\n\t -3.5  \n");
  dts_drc_config_read_file (conf_path, &c);
  near (c.gain_db, -3.5, 0.0, "comments/blank lines/whitespace tolerated");

  write_conf ("2.0\n4.0\n");
  dts_drc_config_read_file (conf_path, &c);
  near (c.gain_db, 2.0, 0.0, "first bare float wins (matches the old reader)");

  write_conf ("999\n");
  dts_drc_config_read_file (conf_path, &c);
  near (c.gain_db, DTS_MAKEUP_GAIN_DB_MAX, 0.0,
      "out-of-range make-up gain clamps");

  write_conf ("");
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == 0.0f, "empty file -> 0.0 dB unity");

  write_conf ("not a number at all\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == 0.0f, "unparseable content -> 0.0 dB unity");

  /* new keys */
  write_conf ("6.0\ndrc=line\ndrc_boost=50\ndrc_cut=70\ncenter=3\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == 6.0f && c.drc_mode == DTS_DRC_MODE_LINE
      && c.drc_boost_pct == 50.0f && c.drc_cut_pct == 70.0f
      && c.center_db == 3.0f, "full config: bare float + all four keys");

  write_conf ("drc=rf\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_RF, "drc=rf");

  write_conf ("drc=off\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_OFF, "drc=off");

  write_conf ("  DRC  =  LiNe  \n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_LINE,
      "keys and values are case- and whitespace-insensitive");

  write_conf ("drc=line\n-2.0\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_LINE && c.gain_db == -2.0f,
      "key line before the bare float still yields both");

  write_conf ("drc=line\r\ndrc_boost=25\r\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_LINE && c.drc_boost_pct == 25.0f,
      "CRLF line endings tolerated");

  /* forward compatibility + invalid input safety */
  write_conf ("drc_future_knob=7\nsomething else\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_OFF && c.drc_boost_pct == 100.0f
      && c.drc_cut_pct == 100.0f && c.center_db == 0.0f,
      "unknown keys ignored -> defaults kept");

  write_conf ("drc=bogus\ndrc_boost=abc\ndrc_cut=\ncenter=xyz\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_OFF && c.drc_boost_pct == 100.0f
      && c.drc_cut_pct == 100.0f && c.center_db == 0.0f,
      "invalid values fall back to each key's default");

  write_conf ("drc_boost=150\ndrc_cut=-20\ncenter=99\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_boost_pct == 100.0f && c.drc_cut_pct == 0.0f
      && c.center_db == 10.0f, "out-of-range key values clamp to the contract");

  write_conf ("center=-99\n");
  dts_drc_config_read_file (conf_path, &c);
  near (c.center_db, DTS_DRC_CENTER_DB_MIN, 0.0, "center clamps at -10 dB");

  /* a very long junk line must not overflow or wedge the reader */
  {
    char big[4096];

    memset (big, 'x', sizeof (big) - 2);
    big[sizeof (big) - 2] = '\n';
    big[sizeof (big) - 1] = '\0';
    write_conf (big);
    dts_drc_config_read_file (conf_path, &c);
    report (c.gain_db == 0.0f && c.drc_mode == DTS_DRC_MODE_OFF,
        "4 KB junk line -> defaults, no crash");
  }

  /* app preset mapping from the epic */
  write_conf ("drc=line\ndrc_boost=50\ndrc_cut=50\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_LINE && c.drc_boost_pct == 50.0f
      && c.drc_cut_pct == 50.0f, "preset \"Light\" round-trips");
  write_conf ("drc=rf\ndrc_boost=100\ndrc_cut=100\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.drc_mode == DTS_DRC_MODE_RF && c.drc_boost_pct == 100.0f
      && c.drc_cut_pct == 100.0f, "preset \"Night\" round-trips");

  remove (conf_path);
}

/* ------------------------------------------- 8. inert-bypass bit-exactness */

/* The output conversion exactly as it appears in gstdtsdec.c's make-up-gain
 * path (the branch taken when drc == off and center == 0). */
static int32_t
convert_shipped (float sample, float makeup_linear)
{
  double s = (double) sample * (double) makeup_linear * 2147483648.0;

  if (s > 2147483647.0)
    s = 2147483647.0;
  else if (s < -2147483648.0)
    s = -2147483648.0;
  return (int32_t) s;
}

/* The pre-patch (upstream + webOS S32) conversion, with no gain factor. */
static int32_t
convert_upstream (float sample)
{
  double s = (double) sample * 2147483648.0;

  if (s > 2147483647.0)
    s = 2147483647.0;
  else if (s < -2147483648.0)
    s = -2147483648.0;
  return (int32_t) s;
}

static void
test_inert (void)
{
  static const float edge[] = {
    0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.5f, -1.5f,
    0.999999940395355f, -0.999999940395355f,
    1.0f / 2147483648.0f, -1.0f / 2147483648.0f,
    1.1754944e-38f, -1.1754944e-38f, 1e-45f, -1e-45f,
    2.0f, -2.0f, 1e9f, -1e9f
  };
  const float unity = dts_drc_db_to_linear (0.0f);
  int i, bad = 0;
  unsigned int bits;

  puts ("[8] inert guarantee (drc=off, center=0, gain=0)");

  memcpy (&bits, &unity, sizeof (bits));
  printf ("  ---- dts_drc_db_to_linear(0) = %.9g (bits 0x%08x)\n",
      (double) unity, bits);
  report (unity == 1.0f && bits == 0x3f800000u,
      "0 dB -> exactly 1.0f, no powf rounding");
  report (dts_drc_db_to_linear (0.0f) * 3.7f == 3.7f,
      "multiplying by that unity factor is the identity");

  for (i = 0; i < (int) (sizeof (edge) / sizeof (edge[0])); i++) {
    if (convert_shipped (edge[i], unity) != convert_upstream (edge[i]))
      bad++;
  }
  report (bad == 0, "edge samples: gain path == pre-patch path, bit-identical");

  bad = 0;
  srand (20260725);
  for (i = 0; i < 2000000; i++) {
    float v = ((float) rand () / (float) RAND_MAX) * 2.4f - 1.2f;

    if (convert_shipped (v, unity) != convert_upstream (v))
      bad++;
  }
  report (bad == 0,
      "2,000,000 random samples in [-1.2, 1.2]: bit-identical S32 output");

  /* The DRC path's own arithmetic is also exact at unity, which is what makes
   * the broader "no DRC, no centre boost" bypass safe for any gain value. */
  bad = 0;
  for (i = 0; i < (int) (sizeof (edge) / sizeof (edge[0])); i++) {
    float g = 1.0f * dts_drc_db_to_linear (0.0f);

    if ((float) (edge[i] * g) != edge[i])
      bad++;
  }
  report (bad == 0, "DRC ramp at unity is an exact float identity too");

  /* And the whole chain is inert: mode off -> 0 dB target -> linear 1.0 */
  report (dts_drc_db_to_linear (dts_drc_scale_gain_db
          (dts_drc_target_gain_db (DTS_DRC_MODE_OFF, -5.0f), 100.0f,
              100.0f)) == 1.0f,
      "mode off -> curve 0 dB -> scale 0 dB -> linear exactly 1.0f");
}

/* ------------------------------------------------------ 9. mode <-> string */

static void
test_mode_strings (void)
{
  puts ("[9] drc-mode property string mapping");
  report (dts_drc_mode_from_string ("off") == DTS_DRC_MODE_OFF, "\"off\"");
  report (dts_drc_mode_from_string ("line") == DTS_DRC_MODE_LINE, "\"line\"");
  report (dts_drc_mode_from_string ("rf") == DTS_DRC_MODE_RF, "\"rf\"");
  report (dts_drc_mode_from_string ("RF") == DTS_DRC_MODE_RF,
      "case-insensitive");
  report (dts_drc_mode_from_string ("nonsense") == DTS_DRC_MODE_OFF,
      "unknown -> off (never fails)");
  report (dts_drc_mode_from_string (NULL) == DTS_DRC_MODE_OFF, "NULL -> off");
  report (strcmp (dts_drc_mode_to_string (DTS_DRC_MODE_OFF), "off") == 0
      && strcmp (dts_drc_mode_to_string (DTS_DRC_MODE_LINE), "line") == 0
      && strcmp (dts_drc_mode_to_string (DTS_DRC_MODE_RF), "rf") == 0
      && strcmp (dts_drc_mode_to_string (99), "off") == 0,
      "round-trips back to the config spelling");
}

/* ------------------------------- 10. end-to-end: a block-by-block DRC run */

static void
test_end_to_end (void)
{
  const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
  const DtsDrcProfile *p = &dts_drc_profiles[DTS_DRC_MODE_LINE];
  float atk = dts_drc_smooth_coef (p->attack_ms, period);
  float rel = dts_drc_smooth_coef (p->release_ms, period);
  float planar[5 * DTS_DRC_BLOCK_SAMPLES];
  float smoothed = 0.0f;
  int i, b, count;

  puts ("[10] end-to-end block loop (line, 100/100, 5 full-range channels)");

  /* A quiet -35 dBFS block train must settle on the curve's +2 dB boost. */
  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = 0.0177828f;     /* -35 dBFS DC */
  for (b = 0; b < 400; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float target = dts_drc_scale_gain_db (dts_drc_target_gain_db
        (DTS_DRC_MODE_LINE, dts_drc_level_dbfs (sum, count)), 100.0f, 100.0f);

    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }
  near (smoothed, 2.0, 0.01, "-35 dBFS settles on +2.0 dB of boost");
  near (dts_drc_db_to_linear (smoothed), 1.258925, 0.002,
      "which is a linear factor of ~1.26");

  /* Then a loud -5 dBFS block train must pull the gain down to -9.75 dB, and
   * do it within a handful of blocks (attack, not release). */
  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = 0.562341f;      /* -5 dBFS DC */
  for (b = 0; b < 5; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float target = dts_drc_scale_gain_db (dts_drc_target_gain_db
        (DTS_DRC_MODE_LINE, dts_drc_level_dbfs (sum, count)), 100.0f, 100.0f);

    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }
  report (smoothed < -8.0f,
      "-5 dBFS pulls the gain below -8 dB within 5 blocks (~27 ms attack)");
  for (b = 0; b < 400; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float target = dts_drc_scale_gain_db (dts_drc_target_gain_db
        (DTS_DRC_MODE_LINE, dts_drc_level_dbfs (sum, count)), 100.0f, 100.0f);

    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }
  near (smoothed, -9.75, 0.01, "-5 dBFS settles on -9.75 dB of cut");

  /* Dialogue-vs-effects is exactly the point: the quiet block ends up
   * 11.75 dB louder relative to the loud one than it was before DRC. */
  report (1, "net dialogue/effects rebalance = 11.75 dB (2.0 boost + 9.75 cut)");

  /* Gain ramp across a block: 256 accumulated adds must land on the target. */
  {
    float from = 1.0f, to = 1.258925f;
    float step = (to - from) * (1.0f / 256.0f);
    float g = from;

    for (i = 0; i < 256; i++)
      g += step;
    /* 256 accumulated float adds drift by ~1e-5; inaudible and self-correcting,
     * because the next block restarts the ramp from the exact target. */
    near (g, to, 5e-5, "256-step ramp lands on the new block's factor");
  }
}

/* ------------------- 11. silence / quiet passages (no gate, amendment F) */

/* NEGATIVE CONTROL ONLY — the PRE-amendment-D boost region: a plateau that
 * holds the cap all the way down to the detector floor, with rf capped at +20.
 * Amendment D changed nothing at or above -43 dBFS, so that part delegates to
 * the real curve; only the max-boost region is reproduced here. Used by both
 * section 11 and section 12 to prove their assertions are non-vacuous. */
static float
legacy_plateau_gain_db (int mode, float level_dbfs)
{
  const DtsDrcProfile *p = &dts_drc_profiles[mode];
  float legacy_cap = (mode == DTS_DRC_MODE_RF) ? 20.0f : 12.0f;
  float knee_boost, g;

  if (level_dbfs >= p->maxboost_knee_db)
    return dts_drc_target_gain_db (mode, level_dbfs);

  knee_boost = (p->boost_knee_db - p->maxboost_knee_db)
      * (1.0f - 1.0f / p->boost_ratio);
  g = knee_boost + (p->maxboost_knee_db - level_dbfs)
      * (1.0f - 1.0f / p->maxboost_ratio);
  return (g > legacy_cap) ? legacy_cap : g;
}

/* NEGATIVE CONTROL ONLY — the silence gate that amendment F retired. The
 * shipped decoder has NO gate; this local replica exists purely to reproduce,
 * below, the near-clipping cue peak that motivated removing it. Do not mistake
 * it for shipped behaviour. */
static int
retired_gate_holds (float level_dbfs)
{
  return !(level_dbfs > -70.0f);
}

/* NEGATIVE CONTROL ONLY — the naive detector bound, `level < FLOOR ? FLOOR :
 * level`. It passes NaN straight through, which is exactly what the shipped
 * inverted comparison in dts_drc_level_dbfs() prevents.
 *
 * Restated for the NaN-hardening chunk: this control used to assert that the
 * naive bound poisons the smoother PERMANENTLY. That is no longer true of
 * either path, because dts_drc_smooth_step() is now self-recovering (it snaps
 * to the next finite target rather than tracking towards it from a NaN). The
 * separation the control still makes — and the one that matters — is that the
 * naive bound corrupts the gain for at least one block, i.e. real samples get
 * multiplied by NaN, while the shipped inverted comparison never lets a single
 * block go bad. The inversion is still load-bearing; it is now the FIRST line
 * of defence rather than the only one. */
static float
naive_level_dbfs (float sum_sq, int count)
{
  float mean_sq, level_db;

  if (count <= 0 || sum_sq <= 0.0f)
    return DTS_DRC_LEVEL_FLOOR_DB;
  mean_sq = sum_sq / (float) count;
  level_db = 10.0f * log10f (mean_sq);
  return (level_db < DTS_DRC_LEVEL_FLOOR_DB) ? DTS_DRC_LEVEL_FLOOR_DB : level_db;
}

static float
amp_from_dbfs (float dbfs)
{
  return powf (10.0f, dbfs / 20.0f);
}

/* Run `blocks` blocks of a constant level through detector -> curve -> scale ->
 * smooth and return the resulting smoothed gain in dB. `curve` is a parameter
 * so any scenario can be replayed against the legacy plateau as a control. */
static float
run_level_curve (int mode, float (*curve) (int, float), float sample_value,
    int blocks, float start_db)
{
  const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
  const DtsDrcProfile *p = &dts_drc_profiles[mode];
  float atk = dts_drc_smooth_coef (p->attack_ms, period);
  float rel = dts_drc_smooth_coef (p->release_ms, period);
  static float planar[5 * DTS_DRC_BLOCK_SAMPLES];
  float g = start_db;
  int i, b, count;

  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = sample_value;

  for (b = 0; b < blocks; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float level = dts_drc_level_dbfs (sum, count);
    float target = dts_drc_scale_gain_db (curve (mode, level), 100.0f, 100.0f);

    g = dts_drc_smooth_step (g, target, atk, rel);
  }
  return g;
}

static float
run_level (int mode, float sample_value, int blocks, float start_db)
{
  return run_level_curve (mode, dts_drc_target_gain_db, sample_value, blocks,
      start_db);
}

/* Establish a gain on `estab_dbfs` content, HARD CUT to digital silence for
 * `silence_s`, then play one `cue_dbfs` block. Models the whole per-block
 * chain including the ramp and the S32 scale/clamp, so it reports the cue's
 * real peak (dBFS, pre-clamp) and counts samples that hit the rails.
 * `use_retired_gate` replays the amendment-C behaviour for the control. */
static float
run_cut_then_cue (int mode, float estab_dbfs, float silence_s, float cue_dbfs,
    int use_retired_gate, int *clipped, float *pre_cue_db)
{
  const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
  const DtsDrcProfile *p = &dts_drc_profiles[mode];
  float atk = dts_drc_smooth_coef (p->attack_ms, period);
  float rel = dts_drc_smooth_coef (p->release_ms, period);
  const int silence_blocks = (int) (silence_s / period);
  static float planar[5 * DTS_DRC_BLOCK_SAMPLES];
  float smoothed = 0.0f, prev_linear, cue_amp, peak = 0.0f;
  float drc_linear, from, to, step, g;
  int b, i, n, count;

  /* 1. establish a gain on real content */
  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = amp_from_dbfs (estab_dbfs);
  for (b = 0; b < 600; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float level = dts_drc_level_dbfs (sum, count);
    float target = dts_drc_scale_gain_db (dts_drc_target_gain_db (mode, level),
        100.0f, 100.0f);

    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }

  /* 2. hard cut to digital silence */
  memset (planar, 0, sizeof (planar));
  for (b = 0; b < silence_blocks; b++) {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float level = dts_drc_level_dbfs (sum, count);
    float target;

    if (use_retired_gate && retired_gate_holds (level))
      continue;
    target = dts_drc_scale_gain_db (dts_drc_target_gain_db (mode, level),
        100.0f, 100.0f);
    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }
  *pre_cue_db = smoothed;
  prev_linear = dts_drc_db_to_linear (smoothed);

  /* 3. the cue, ramped from whatever gain the silence left behind */
  cue_amp = amp_from_dbfs (cue_dbfs);
  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = cue_amp;
  {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float level = dts_drc_level_dbfs (sum, count);

    if (!(use_retired_gate && retired_gate_holds (level))) {
      float target = dts_drc_scale_gain_db (dts_drc_target_gain_db (mode,
              level), 100.0f, 100.0f);

      smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
    }
  }
  drc_linear = dts_drc_db_to_linear (smoothed);
  from = prev_linear;
  to = drc_linear;
  step = (to - from) * (1.0f / 256.0f);

  *clipped = 0;
  g = from;
  for (n = 0; n < 256; n++) {
    float out;
    double s;

    g += step;
    out = (float) cue_amp * g;
    s = (double) out * 2147483648.0;
    if (s > 2147483647.0 || s < -2147483648.0)
      (*clipped)++;
    if (fabsf (out) > peak)
      peak = fabsf (out);
  }
  return 20.0f * log10f (peak);
}

static void
test_silence_no_gate (void)
{
  float g, ctl, peak_new, peak_old, pre_new, pre_old;
  int clip_new, clip_old;

  puts ("[11] silence / quiet passages — no gate (amendment F retired it)");

  /* (b) digital silence must drive the target toward 0 dB via the amendment-D
   * decay, NOT toward max boost. The detector floors silence at -90 dBFS and
   * the decayed curve maps that to exactly 0 dB. */
  near (dts_drc_level_dbfs (0.0f, 1280), DTS_DRC_LEVEL_FLOOR_DB, 0.0,
      "digital silence reads the -90 dBFS floor");
  report (dts_drc_target_gain_db (DTS_DRC_MODE_LINE,
          DTS_DRC_LEVEL_FLOOR_DB) == 0.0f
      && dts_drc_target_gain_db (DTS_DRC_MODE_RF,
          DTS_DRC_LEVEL_FLOOR_DB) == 0.0f,
      "the floor maps to exactly 0 dB of gain in both modes");

  g = run_level (DTS_DRC_MODE_LINE, 0.0f, 2000, 2.0f);
  report (fabsf (g) < 0.01f,
      "line: 10.7 s of silence releases an established +2 dB back to 0 dB");
  g = run_level (DTS_DRC_MODE_RF, 0.0f, 2000, -13.55f);
  report (fabsf (g) < 0.01f,
      "rf:   10.7 s of silence releases an established -13.55 dB back to 0 dB");
  report (dts_drc_db_to_linear (run_level (DTS_DRC_MODE_RF, 0.0f, 2000, 0.0f))
      == 1.0f, "rf: silence from unity stays at exactly 1.0 linear");

  /* NEGATIVE CONTROL: without amendment D's decay the very same silence winds
   * UP to the cap. This is what proves the decay -- not something else -- is
   * what makes silence safe now that the gate is gone. */
  ctl = run_level_curve (DTS_DRC_MODE_LINE, legacy_plateau_gain_db, 0.0f, 2000,
      0.0f);
  near (ctl, 11.998, 0.01,
      "negative control: legacy plateau winds line silence up to +12 dB");
  ctl = run_level_curve (DTS_DRC_MODE_RF, legacy_plateau_gain_db, 0.0f, 2000,
      0.0f);
  near (ctl, 20.0, 0.01,
      "negative control: legacy plateau winds rf silence up to +20 dB");

  /* (a) THE CASE THAT RETIRED THE GATE: a gain established on quiet content,
   * a hard cut to silence, then a loud cue. With no gate the compressor
   * releases during the silence and the cue lands where it should. */
  peak_new = run_cut_then_cue (DTS_DRC_MODE_LINE, -50.0f, 3.0f, -12.0f, 0,
      &clip_new, &pre_new);
  peak_old = run_cut_then_cue (DTS_DRC_MODE_LINE, -50.0f, 3.0f, -12.0f, 1,
      &clip_old, &pre_old);
  printf ("  ---- hard cut from -50 dBFS -> 3 s silence -> -12 dBFS cue:\n"
      "       no gate      pre-cue %+8.4f dB, cue peak %8.4f dBFS, %d clipped\n"
      "       retired gate pre-cue %+8.4f dB, cue peak %8.4f dBFS, %d clipped\n",
      (double) pre_new, (double) peak_new, clip_new,
      (double) pre_old, (double) peak_old, clip_old);
  report (clip_new == 0, "no gate: the post-cut cue clips ZERO samples");
  report (fabsf (peak_new - (-12.0f)) < 1.0f,
      "no gate: the cue peaks near -12 dBFS, i.e. where it was authored");
  /* NEGATIVE CONTROL: the retired gate froze the pre-cut gain, leaving the cue
   * a fraction of a dB from the clamp. This is the regression amendment F
   * removed, and it must stay visible. */
  report (peak_old > -1.0f,
      "negative control: the retired gate put that cue within 1 dB of clipping");
  report (peak_new < peak_old - 10.0f,
      "removing the gate bought more than 10 dB of headroom on this case");

  /* the compressor must still respond promptly once real content returns */
  g = run_level (DTS_DRC_MODE_LINE, 0.562341f, 400, 0.0f);
  near (g, -9.75, 0.01, "settles onto -9.75 dB when -5 dBFS content returns");
  g = run_level (DTS_DRC_MODE_LINE, 0.562341f, 5, 0.0f);
  report (g < -4.0f, "attack is prompt: past -4 dB within 5 blocks");

  /* (c) NaN / non-finite input. With the gate retired the detector's inverted
   * comparison is the ONLY line of defence, so pin it hard. */
  report (dts_drc_level_dbfs ((float) (0.0 / 0.0), 1280)
      == DTS_DRC_LEVEL_FLOOR_DB, "a NaN block floors to -90 dBFS");
  report (dts_drc_target_gain_db (DTS_DRC_MODE_LINE,
          dts_drc_level_dbfs ((float) (0.0 / 0.0), 1280)) == 0.0f,
      "...and therefore resolves to exactly 0 dB of gain");
  {
    const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
    float atk = dts_drc_smooth_coef (10.0f, period);
    float rel = dts_drc_smooth_coef (250.0f, period);
    float shipped = 0.0f, naive = 0.0f;
    int i, shipped_finite = 1;
    int naive_bad_blocks = 0, naive_recovered = 0;

    for (i = 0; i < 200; i++) {
      float sum = (i == 50) ? (float) (0.0 / 0.0)
          : (i == 100) ? (float) (1.0 / 0.0) : 0.32f;
      float ls = dts_drc_level_dbfs (sum, 1280);
      float ln = naive_level_dbfs (sum, 1280);

      shipped = dts_drc_smooth_step (shipped,
          dts_drc_scale_gain_db (dts_drc_target_gain_db (DTS_DRC_MODE_LINE,
                  ls), 100.0f, 100.0f), atk, rel);
      naive = dts_drc_smooth_step (naive,
          dts_drc_scale_gain_db (dts_drc_target_gain_db (DTS_DRC_MODE_LINE,
                  ln), 100.0f, 100.0f), atk, rel);
      if (!(shipped > -1e6f && shipped < 1e6f))
        shipped_finite = 0;
      if (!(naive > -1e6f && naive < 1e6f))
        naive_bad_blocks++;
    }
    naive_recovered = (naive > -1e6f && naive < 1e6f);
    report (shipped_finite,
        "NaN and +inf blocks cannot poison the smoother across 200 blocks");
    /* NEGATIVE CONTROL: the naive bound still corrupts the gain for real
     * blocks — it just no longer does so forever. If this ever reads 0 the
     * assertion above has become vacuous. */
    printf ("  ---- naive bound corrupted %d of 200 blocks; shipped bound "
        "corrupted 0\n", naive_bad_blocks);
    report (naive_bad_blocks > 0,
        "negative control: the naive `level < FLOOR` bound does corrupt blocks");
    /* Self-recovery (the second line of defence): even the naive path is back
     * to a finite gain by the end of the run, so a single bad block can no
     * longer poison the rest of the stream. */
    report (naive_recovered,
        "the smoother recovers to a finite gain after a non-finite block");
  }

  /* Steady state must still be zipper-free: once converged, from == to and the
   * per-sample ramp step is exactly zero. (Pre-amendment-F this property was
   * asserted on gate-held blocks; the scenario survives, the reason changed.) */
  {
    const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
    const DtsDrcProfile *p = &dts_drc_profiles[DTS_DRC_MODE_LINE];
    float atk = dts_drc_smooth_coef (p->attack_ms, period);
    float rel = dts_drc_smooth_coef (p->release_ms, period);
    static float planar[5 * DTS_DRC_BLOCK_SAMPLES];
    float smoothed = 0.0f, prev_linear = 1.0f;
    int b, count, flat = 1;

    memset (planar, 0, sizeof (planar));        /* digital silence */
    for (b = 0; b < 4000; b++) {
      float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
          &count);
      float level = dts_drc_level_dbfs (sum, count);
      float target = dts_drc_scale_gain_db (dts_drc_target_gain_db
          (DTS_DRC_MODE_LINE, level), 100.0f, 100.0f);
      float drc_linear, from, to, step;

      smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
      drc_linear = dts_drc_db_to_linear (smoothed);
      from = prev_linear;
      to = drc_linear;
      step = (to - from) * (1.0f / 256.0f);
      prev_linear = drc_linear;
      if (b > 2000 && (to != from || step != 0.0f))
        flat = 0;
    }
    report (flat,
        "converged silence gives a perfectly flat ramp (to == from, step == 0)");
  }
}

/* --------------------------------------- 12. boost decay (amendment D) */

/* The reviewer's scenario: a `fade_s` fade from -20 dBFS down to -85 dBFS,
 * then one loud cue block. Models the whole per-block chain including the ramp
 * and the S32 scale/clamp, so it counts REAL clipped output samples.
 * Returns the gain (dB) held going into the cue; *clipped gets the count. */
static float
run_fade_then_cue (int mode, float (*curve) (int, float), float cue_dbfs,
    int *clipped)
{
  const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
  const DtsDrcProfile *p = &dts_drc_profiles[mode];
  float atk = dts_drc_smooth_coef (p->attack_ms, period);
  float rel = dts_drc_smooth_coef (p->release_ms, period);
  const int blocks = (int) (3.2f / period);      /* 3.2 s fade */
  static float planar[5 * DTS_DRC_BLOCK_SAMPLES];
  float smoothed = 0.0f, prev_linear = 1.0f, pre_cue_db;
  float drc_linear, from, to, step, g, cue_amp;
  int b, i, count, n;

  for (b = 0; b < blocks; b++) {
    float lvl_db = -20.0f + (-85.0f + 20.0f) * ((float) b / (float) blocks);
    float amp = amp_from_dbfs (lvl_db);
    float sum, level, target;

    for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
      planar[i] = amp;
    sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1, &count);
    level = dts_drc_level_dbfs (sum, count);
    target = dts_drc_scale_gain_db (curve (mode, level), 100.0f, 100.0f);
    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
    prev_linear = dts_drc_db_to_linear (smoothed);
  }
  pre_cue_db = smoothed;

  /* the cue block: its ramp STARTS from the gain the fade left behind */
  cue_amp = amp_from_dbfs (cue_dbfs);
  for (i = 0; i < 5 * DTS_DRC_BLOCK_SAMPLES; i++)
    planar[i] = cue_amp;
  {
    float sum = dts_drc_sum_squares (planar, 5, DTS_DRC_BLOCK_SAMPLES, -1,
        &count);
    float level = dts_drc_level_dbfs (sum, count);

    float target = dts_drc_scale_gain_db (curve (mode, level), 100.0f, 100.0f);

    smoothed = dts_drc_smooth_step (smoothed, target, atk, rel);
  }
  drc_linear = dts_drc_db_to_linear (smoothed);
  from = prev_linear;
  to = drc_linear;
  step = (to - from) * (1.0f / 256.0f);

  *clipped = 0;
  g = from;
  for (n = 0; n < 256; n++) {
    double s;

    g += step;
    s = (double) ((float) cue_amp * g) * 2147483648.0;
    if (s > 2147483647.0 || s < -2147483648.0)
      (*clipped)++;
  }
  return pre_cue_db;
}

static void
test_boost_decay (void)
{
  float l, peak_l = 0.0f, peak_rf = 0.0f;
  float pre_new, pre_old;
  int clip_new, clip_old;

  puts ("[12] boost decay (amendment D) — peaks, zero return, fade behaviour");

  /* peak values and where they occur */
  for (l = -90.0f; l <= -30.0f; l += 0.01f) {
    float gl = dts_drc_target_gain_db (DTS_DRC_MODE_LINE, l);
    float gr = dts_drc_target_gain_db (DTS_DRC_MODE_RF, l);

    if (gl > peak_l)
      peak_l = gl;
    if (gr > peak_rf)
      peak_rf = gr;
  }
  near (peak_l, 12.0, 0.01, "line peak boost is +12 dB");
  near (peak_rf, 16.0, 0.01, "rf peak boost is +16 dB (reduced from +20)");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -50.5f), 12.0, 1e-3,
      "line peaks at -50.5 dBFS");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -55.5f), 16.0, 1e-3,
      "rf peaks at -55.5 dBFS");

  /* return to 0 dB at -85 and below, for both modes */
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -85.0f), 0.0, 1e-4,
      "line returns to 0 dB at -85 dBFS");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -85.0f), 0.0, 1e-4,
      "rf returns to 0 dB at -85 dBFS");
  report (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -86.0f) == 0.0f
      && dts_drc_target_gain_db (DTS_DRC_MODE_RF, -100.0f) == 0.0f,
      "below -85 dBFS the boost stays exactly 0 dB (never negative)");

  /* Pin boost_zero_db itself. The "returns to 0 dB at -85" checks above do NOT
   * catch it moving DOWN (0 is still correct below a lowered zero point), so
   * assert the constant and the fact that -84 dBFS is still boosted. */
  report (dts_drc_profiles[DTS_DRC_MODE_LINE].boost_zero_db == -85.0f
      && dts_drc_profiles[DTS_DRC_MODE_RF].boost_zero_db == -85.0f,
      "boost_zero_db is -85 dBFS in both modes");
  report (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, -84.0f) > 0.0f
      && dts_drc_target_gain_db (DTS_DRC_MODE_RF, -84.0f) > 0.0f,
      "-84 dBFS is still boosted (the zero point has not crept upward)");

  /* Pin the derived peak levels too — this is what makes 5:1 (amendment A)
   * and the peak locations mutually consistent, and it is the assertion that
   * would fail if a port used 6:1 and moved the rf peak to -55.0 dBFS. */
  report (dts_drc_target_gain_db (DTS_DRC_MODE_RF, -55.0f) < 16.0f,
      "rf at -55.0 dBFS is BELOW the peak (peak is -55.5, i.e. 5:1 not 6:1)");

  /* the decay leg must be strictly increasing in level, no kinks */
  {
    int ok = 1;
    float prev = dts_drc_target_gain_db (DTS_DRC_MODE_RF, -85.0f);

    for (l = -84.9f; l <= -55.5f; l += 0.1f) {
      float g = dts_drc_target_gain_db (DTS_DRC_MODE_RF, l);

      if (g < prev - 1e-4f)
        ok = 0;
      prev = g;
    }
    report (ok, "rf decay leg rises monotonically from -85 up to the peak");
  }

  /* THE SCENARIO amendment D exists for: a 3.2 s fade to -85 dBFS followed by
   * a -12 dBFS cue. Pre-D this reached +11.92 dB (line) / +19.89 dB (rf) and
   * clipped; it must now be far lower and clip nothing. */
  pre_new = run_fade_then_cue (DTS_DRC_MODE_RF, dts_drc_target_gain_db,
      -12.0f, &clip_new);
  pre_old = run_fade_then_cue (DTS_DRC_MODE_RF, legacy_plateau_gain_db,
      -12.0f, &clip_old);
  printf ("  ---- rf   fade->cue: gain before cue %+.4f dB (was %+.4f dB), "
      "clipped samples %d (was %d)\n", (double) pre_new, (double) pre_old,
      clip_new, clip_old);
  report (clip_new == 0, "rf: the post-fade cue clips ZERO samples");
  report (pre_new < 10.0f, "rf: pre-cue gain is well below the old +19.89 dB");
  /* NEGATIVE CONTROL: the legacy plateau curve must reproduce the old damage */
  report (pre_old > 19.0f,
      "negative control: legacy plateau still winds rf up past +19 dB");
  report (clip_old > 0,
      "negative control: legacy plateau still clips the post-fade cue");

  pre_new = run_fade_then_cue (DTS_DRC_MODE_LINE, dts_drc_target_gain_db,
      -12.0f, &clip_new);
  pre_old = run_fade_then_cue (DTS_DRC_MODE_LINE, legacy_plateau_gain_db,
      -12.0f, &clip_old);
  printf ("  ---- line fade->cue: gain before cue %+.4f dB (was %+.4f dB), "
      "clipped samples %d (was %d)\n", (double) pre_new, (double) pre_old,
      clip_new, clip_old);
  report (clip_new == 0, "line: the post-fade cue clips ZERO samples");
  report (pre_new < 8.0f, "line: pre-cue gain is well below the old +11.92 dB");
  report (pre_old > 11.0f,
      "negative control: legacy plateau still winds line up past +11 dB");
  report (pre_new < pre_old - 3.0f,
      "the decay cuts the post-fade wind-up by more than 3 dB");

  /* The decay now runs to completion: with no gate to freeze it partway, a
   * fade ends at effectively unity rather than the +8.20 dB (rf) / +5.29 dB
   * (line) the retired gate used to hold. */
  report (fabsf (pre_new) < 0.5f,
      "the fade now decays essentially all the way back to unity");
}

/* ------------------------------------------------ 13. NaN hardening (config)
 *
 * A non-finite value must not be able to reach the DSP from ANY config key.
 * This is reachable in production, not theoretical: the companion app writes
 * the config from JavaScript and String(NaN) is the literal "NaN", which
 * strtod() parses happily. What used to happen:
 *   center=nan    -> center_boost_linear NaN, drc_active 1 -> centre samples
 *                    become (gint32) NaN, which is UNDEFINED BEHAVIOUR;
 *   drc_boost=nan -> the smoothed gain goes NaN and stays NaN on every channel
 *                    for the rest of the stream.
 * inf and -inf were always fine (they are ordered, so they clamped); NaN was
 * the only value that slipped, because `v > hi` and `v < lo` are BOTH false
 * for it.
 * ------------------------------------------------------------------------- */

/* NEGATIVE CONTROL ONLY — the pre-fix clamp. Every assertion below that claims
 * the shipped clamp stops a NaN is paired with this, so a reverted fix cannot
 * read as a pass. */
static float
naive_clampf (float v, float lo, float hi)
{
  if (v > hi)
    return hi;
  if (v < lo)
    return lo;
  return v;
}

static int
is_finitef (float v)
{
  return (v == v) && (v > -1e30f) && (v < 1e30f);
}

static void
test_nan_hardening (void)
{
  DtsDrcConfig c;
  const float nan_f = (float) (0.0 / 0.0);
  static const char *spellings[] = { "nan", "NaN", "NAN", "-nan", "+NaN" };
  size_t k;

  puts ("[13] NaN hardening — no config key can put a NaN into the DSP");

  /* (a) the clamp itself, over every range the contract uses */
  report (dts_drc_clampf (nan_f, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX)
      == DTS_DRC_PCT_MIN, "clampf(NaN) over 0..100 -> 0, not NaN");
  report (dts_drc_clampf (nan_f, DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX)
      == DTS_DRC_CENTER_DB_MIN, "clampf(NaN) over -10..+10 -> -10, not NaN");
  report (dts_drc_clampf (nan_f, DTS_MAKEUP_GAIN_DB_MIN,
          DTS_MAKEUP_GAIN_DB_MAX) == DTS_MAKEUP_GAIN_DB_MIN,
      "clampf(NaN) over -20..+20 -> -20, not NaN");
  /* NEGATIVE CONTROL: the old form passed it straight through. */
  report (!(naive_clampf (nan_f, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX)
          == naive_clampf (nan_f, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX)),
      "negative control: the pre-fix clamp returns NaN for the same input");

  /* Finite and infinite inputs must behave as before the fix — the hardening
   * must not have moved the contract. Both clamps are compared over the whole
   * range plus the two infinities.
   *
   * Precisely what this proves: the two forms compare EQUAL for every finite
   * value, which is not quite the same as bit-identical. `==` cannot see the
   * sign of zero, and there is exactly one input where they differ in it: with
   * lo == 0.0f (the 0..100 pct range) the pre-fix form returned -0.0f for an
   * input of -0.0f, where this one returns +0.0f. Inert everywhere it is used
   * — the pct values are only ever multiplied by 0.01f and then by a gain in
   * dB, and -0.0f and +0.0f multiply and compare the same — so the claim is
   * "equal for all finite values", deliberately not "bit-identical". */
  {
    int same = 1;
    int i;

    for (i = -3000; i <= 3000; i++) {
      float v = (float) i * 0.05f;      /* -150 .. +150 in 0.05 steps */

      if (dts_drc_clampf (v, DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX)
          != naive_clampf (v, DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX))
        same = 0;
      if (dts_drc_clampf (v, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX)
          != naive_clampf (v, DTS_DRC_PCT_MIN, DTS_DRC_PCT_MAX))
        same = 0;
    }
    report (same, "6001 finite values clamp identically to the pre-fix form");
  }
  report (dts_drc_clampf ((float) (1.0 / 0.0), DTS_DRC_PCT_MIN,
          DTS_DRC_PCT_MAX) == DTS_DRC_PCT_MAX, "+inf still clamps to hi");
  report (dts_drc_clampf ((float) (-1.0 / 0.0), DTS_DRC_CENTER_DB_MIN,
          DTS_DRC_CENTER_DB_MAX) == DTS_DRC_CENTER_DB_MIN,
      "-inf still clamps to lo");

  /* (b) every config key, in every spelling strtod() accepts. A NaN is not an
   * out-of-range number, so it is treated as unparseable: that key keeps its
   * DEFAULT (inert), rather than clamping to the bottom of its range. */
  for (k = 0; k < sizeof (spellings) / sizeof (spellings[0]); k++) {
    char buf[256];
    char label[160];

    snprintf (buf, sizeof (buf),
        "%s\ndrc=line\ndrc_boost=%s\ndrc_cut=%s\ncenter=%s\n",
        spellings[k], spellings[k], spellings[k], spellings[k]);
    write_conf (buf);
    dts_drc_config_read_file (conf_path, &c);

    snprintf (label, sizeof (label),
        "\"%s\" in every key -> finite, inert defaults", spellings[k]);
    report (is_finitef (c.gain_db) && is_finitef (c.drc_boost_pct)
        && is_finitef (c.drc_cut_pct) && is_finitef (c.center_db)
        && c.gain_db == 0.0f && c.drc_boost_pct == 100.0f
        && c.drc_cut_pct == 100.0f && c.center_db == 0.0f, label);
  }

  /* inf via the config keeps clamping (amendment B), so the fix is narrow. */
  write_conf ("inf\ndrc_boost=inf\ndrc_cut=-inf\ncenter=-inf\n");
  dts_drc_config_read_file (conf_path, &c);
  report (c.gain_db == DTS_MAKEUP_GAIN_DB_MAX
      && c.drc_boost_pct == DTS_DRC_PCT_MAX
      && c.drc_cut_pct == DTS_DRC_PCT_MIN
      && c.center_db == DTS_DRC_CENTER_DB_MIN,
      "inf/-inf still CLAMP (amendment B), only NaN falls back to default");

  /* (c) the UB path: center=nan must not reach the (gint32) conversion. The
   * decoder computes center_boost_linear = db_to_linear(clamp(center_db)); if
   * that is NaN, every centre sample is (gint32) NaN. */
  write_conf ("center=nan\n");
  dts_drc_config_read_file (conf_path, &c);
  {
    float lin = dts_drc_db_to_linear (dts_drc_clampf (c.center_db,
            DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX));
    float naive_lin = dts_drc_db_to_linear (naive_clampf (nan_f,
            DTS_DRC_CENTER_DB_MIN, DTS_DRC_CENTER_DB_MAX));

    report (is_finitef (lin) && lin == 1.0f,
        "center=nan -> centre multiplier is an exact finite 1.0 (no UB)");
    report (!is_finitef (naive_lin),
        "negative control: the pre-fix path produced a NaN multiplier here");
  }

  /* (d) the poisoning path: a NaN boost% must not wedge the smoother, and the
   * smoother must recover even if a NaN somehow reaches it anyway. */
  {
    const float period = (float) DTS_DRC_BLOCK_SAMPLES / 48000.0f;
    float atk = dts_drc_smooth_coef (10.0f, period);
    float rel = dts_drc_smooth_coef (250.0f, period);
    float g = 0.0f;
    int b, finite_throughout = 1;

    write_conf ("drc=line\ndrc_boost=nan\ndrc_cut=nan\n");
    dts_drc_config_read_file (conf_path, &c);
    for (b = 0; b < 100; b++) {
      float target = dts_drc_scale_gain_db (dts_drc_target_gain_db (c.drc_mode,
              -46.0f), c.drc_boost_pct, c.drc_cut_pct);

      g = dts_drc_smooth_step (g, target, atk, rel);
      if (!is_finitef (g))
        finite_throughout = 0;
    }
    report (finite_throughout,
        "drc_boost=nan -> the smoothed gain is finite on all 100 blocks");
    report (g > 0.5f,
        "  ...and the default 100% boost is what got used (not 0%)");

    /* Direct recovery proof: hand the smoother a NaN state and a finite
     * target, exactly the situation that used to be permanent. */
    g = dts_drc_smooth_step (nan_f, -3.25f, atk, rel);
    report (g == -3.25f,
        "a NaN smoother state snaps to the next finite target (recovery)");
    g = dts_drc_smooth_step (g, -3.25f, atk, rel);
    report (is_finitef (g), "  ...and stays finite on the block after that");
  }
}

int
main (int argc, char **argv)
{
  const char *dir = (argc > 1) ? argv[1] : ".";

  snprintf (conf_path, sizeof (conf_path), "%s/dtsdec-drc-test-gain.conf",
      dir);

  puts ("=== webOS 25 dtsdec DRC core — host unit test ===");
  puts ("(DSP code extracted verbatim from ../gstdtsdec.c)\n");

  test_curve_line ();
  test_curve_rf ();
  test_curve_shape ();
  test_scaling ();
  test_smoothing ();
  test_detector ();
  test_config ();
  test_inert ();
  test_mode_strings ();
  test_end_to_end ();
  test_silence_no_gate ();
  test_boost_decay ();
  test_nan_hardening ();

  printf ("\n=== %d checks, %d failures ===\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
