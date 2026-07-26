/* Host unit test for the MLP HOST BINDING of the webOS 25 DRC port.
 *
 * The shared DSP is already covered: run-tests.sh's 146 assertions run against
 * drc-core.inc, and this port embeds that core byte-for-byte. What is NOT
 * shared is the interleaved-integer detector input and the interleaved apply
 * loop, so those are what this file exercises. Both are extracted from the
 * patch heredoc in build-truehd.sh (mlp-binding.inc) rather than copied, so
 * the assertions run against the code that actually ships.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* M_PI is an XSI extension, not C99, so glibc hides it under -std=c99. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_CHANNELS 8          /* libavcodec/mlp.h */

#include "drc-core.inc"
#include "mlp-binding.inc"

static int checks = 0, failures = 0;

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
  printf ("  %-4s %-50s got %12.6f  want %12.6f\n", ok ? "ok" : "FAIL", label,
      got, want);
}

#define NCH 6
#define NSMP 160
#define LFE_CH 3
#define CENTER_CH 2
#define LFE2_CH 5

/* ---------------------------------------------------------------------------
 * 1. Detector parity: the interleaved-integer detector must land on the SAME
 *    dBFS as the DTS side's planar-float detector for the same audio.
 * ------------------------------------------------------------------------ */
static void
test_detector_parity (void)
{
  static float planar[NCH * NSMP];
  static int32_t ilv32[NCH * NSMP];
  static int16_t ilv16[NCH * NSMP];
  const double amps[] = { 0.5, 0.1, 0.01, 0.001, 1.0 };
  size_t k;

  puts ("[1] detector parity — planar float vs interleaved integer");

  for (k = 0; k < sizeof (amps) / sizeof (amps[0]); k++) {
    double a = amps[k];
    int c, n, count_p = 0, count_i = 0;
    float lvl_p, lvl_32, lvl_16;
    char label[96];

    for (c = 0; c < NCH; c++) {
      for (n = 0; n < NSMP; n++) {
        double x = a * sin (2.0 * M_PI * (n + 7 * c) / 32.0);

        planar[c * NSMP + n] = (float) x;
        ilv32[n * NCH + c] = (int32_t) (x * 2147483647.0);
        ilv16[n * NCH + c] = (int16_t) (x * 32767.0);
      }
    }

    /* no channel skipped, so both detectors see the same sample set */
    lvl_p = dts_drc_level_dbfs (dts_drc_sum_squares (planar, NCH, NSMP, -1,
            &count_p), count_p);
    lvl_32 = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv32, NSMP, NCH, -1,
            -1, 1, &count_i), count_i);
    report (count_p == count_i && count_p == NCH * NSMP,
        "sample counts agree");
    snprintf (label, sizeof (label), "amp %.3f: S32 detector == planar float",
        a);
    near (lvl_32, lvl_p, 0.01, label);

    lvl_16 = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv16, NSMP, NCH, -1,
            -1, 0, &count_i), count_i);
    snprintf (label, sizeof (label), "amp %.3f: S16 detector == planar float",
        a);
    /* S16 quantisation is the only difference, and it grows as the signal
     * approaches the 16-bit LSB: 0.001 dB at -3 dBFS, 0.02 dB at -43, 0.1 dB
     * at -63. Irrelevant to the curve (its narrowest feature is an 11 dB
     * band). TrueHD only selects S16 for <= 16-bit streams anyway. */
    near (lvl_16, lvl_p, a >= 0.1 ? 0.01 : (a >= 0.01 ? 0.05 : 0.2), label);
  }
}

/* ---------------------------------------------------------------------------
 * 2. LFE exclusion + the floor / NaN-safety path reached through this input.
 * ------------------------------------------------------------------------ */
static void
test_lfe_exclusion (void)
{
  static int32_t ilv[NCH * NSMP];
  int n, count = 0;
  float lvl;

  puts ("[2] LFE exclusion");

  /* Only the LFE carries signal; everything else is digital silence. */
  memset (ilv, 0, sizeof (ilv));
  for (n = 0; n < NSMP; n++)
    ilv[n * NCH + LFE_CH] = 2147483647;

  lvl = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv, NSMP, NCH, LFE_CH, -1, 1,
          &count), count);
  report (count == (NCH - 1) * NSMP, "LFE samples are not counted");
  near (lvl, DTS_DRC_LEVEL_FLOOR_DB, 1e-4,
      "full-scale LFE alone -> detector floor");
  near (dts_drc_target_gain_db (DTS_DRC_MODE_LINE, lvl), 0.0, 1e-4,
      "  ...and therefore 0 dB of gain (no wind-up on silence)");

  /* Same buffer WITHOUT the exclusion would read far louder — proves the
   * skip argument is load-bearing, not decorative. */
  lvl = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv, NSMP, NCH, -1, -1, 1,
          &count), count);
  report (lvl > -10.0f, "negative control: including the LFE reads loud");

  /* A TrueHD layout can carry a SECOND LFE (AV_CH_LOW_FREQUENCY_2 is in
   * thd_channel_order), which must be excluded too. Put full-scale signal in
   * both LFE slots and check the detector still reads silence. */
  memset (ilv, 0, sizeof (ilv));
  for (n = 0; n < NSMP; n++) {
    ilv[n * NCH + LFE_CH] = 2147483647;
    ilv[n * NCH + LFE2_CH] = 2147483647;
  }
  lvl = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv, NSMP, NCH, LFE_CH,
          LFE2_CH, 1, &count), count);
  report (count == (NCH - 2) * NSMP, "both LFE channels are skipped");
  near (lvl, DTS_DRC_LEVEL_FLOOR_DB, 1e-4,
      "full-scale LFE + LFE2 alone -> detector floor");
  lvl = dts_drc_level_dbfs (mlp_drc_sum_squares_ilv (ilv, NSMP, NCH, LFE_CH,
          -1, 1, &count), count);
  report (lvl > -10.0f,
      "negative control: skipping only LFE1 still reads the LFE2 signal");
}

/* ---------------------------------------------------------------------------
 * 3. Apply loop: bit-exactness at unity, ramp endpoints, centre isolation.
 * ------------------------------------------------------------------------ */
static void
test_apply (void)
{
  static int32_t buf[NCH * NSMP], ref[NCH * NSMP];
  int n, c, ok;

  puts ("[3] interleaved apply loop");

  for (n = 0; n < NSMP; n++)
    for (c = 0; c < NCH; c++)
      /* 24-bit values in a 32-bit container, exactly what
       * ff_mlp_pack_output() writes (sample * 256). */
      ref[n * NCH + c] = ((n * 7919 + c * 104729) % 8388608 - 4194304) * 256;

  /* unity DRC, no centre boost, no make-up gain -> byte-for-byte untouched */
  memcpy (buf, ref, sizeof (buf));
  mlp_drc_apply (buf, NSMP, NCH, 1, 1.0f, 1.0f, -1, 1.0f);
  report (memcmp (buf, ref, sizeof (buf)) == 0,
      "from == to == 1.0, no centre -> output bit-identical to input");

  /* centre boost applied to the centre channel ONLY */
  memcpy (buf, ref, sizeof (buf));
  mlp_drc_apply (buf, NSMP, NCH, 1, 1.0f, 1.0f, CENTER_CH, 2.0f);
  ok = 1;
  for (n = 0; n < NSMP && ok; n++)
    for (c = 0; c < NCH; c++) {
      int32_t want = (c == CENTER_CH)
          ? (int32_t) (double) ((float) ref[n * NCH + c] * 2.0f)
          : ref[n * NCH + c];

      if (buf[n * NCH + c] != want) {
        ok = 0;
        break;
      }
    }
  report (ok, "centre boost x2 hits the centre channel and nothing else");

  /* ramp endpoints: first sample sees from+step, last sees exactly to */
  memcpy (buf, ref, sizeof (buf));
  mlp_drc_apply (buf, NSMP, NCH, 1, 1.0f, 2.0f, -1, 1.0f);
  {
    float step = 1.0f / (float) NSMP;
    int32_t first_want = (int32_t) (double) ((float) ref[0] * (1.0f + step));
    int32_t last_want = (int32_t) (double) ((float) ref[(NSMP - 1) * NCH] *
        2.0f);

    double rel = fabs (buf[(NSMP - 1) * NCH] - (double) last_want)
        / fabs ((double) last_want);

    report (buf[0] == first_want, "ramp: first sample gets from + step");
    /* The accumulated ramp lands on `to` to within float rounding. The DTS
     * side's 1/256 step is dyadic so its accumulation is exact; 1/40, 1/80 and
     * 1/160 are not, so the MLP ramp ends ~2e-6 low/high (0.00002 dB). This
     * cannot drift: cur[] is re-seeded from `from` at every block, and `from`
     * comes from drc_prev_linear, never from cur[]. */
    near (rel * 1e6, 0.0, 5.0,
        "ramp: last sample reaches `to` within 5 ppm (x1e6)");
  }

  /* saturating clamp, both sample formats */
  {
    int32_t big[2] = { 2000000000, -2000000000 };
    int16_t small[2] = { 30000, -30000 };

    mlp_drc_apply (big, 1, 2, 1, 4.0f, 4.0f, -1, 1.0f);
    report (big[0] == 2147483647 && big[1] == -2147483648LL + 0,
        "S32 clamp saturates, never wraps");
    mlp_drc_apply (small, 1, 2, 0, 4.0f, 4.0f, -1, 1.0f);
    report (small[0] == 32767 && small[1] == -32768,
        "S16 clamp saturates, never wraps");
  }
}

/* ---------------------------------------------------------------------------
 * 4. Contract application order, end to end through the real functions:
 *      sample -> x drc -> x center (centre only) -> x makeup -> clamp
 * ------------------------------------------------------------------------ */
static void
test_order (void)
{
  int32_t buf[NCH];
  float drc = dts_drc_db_to_linear (6.0f);
  float ctr = dts_drc_db_to_linear (3.0f);
  float makeup = 2.0f;
  float from, to;
  int c;

  puts ("[4] application order (drc x center x makeup)");

  for (c = 0; c < NCH; c++)
    buf[c] = 1000000;

  /* steady state: prev == current, so from == to and the ramp is flat */
  from = to = drc * makeup;
  mlp_drc_apply (buf, 1, NCH, 1, from, to, CENTER_CH, ctr);

  near (buf[0] / 1000000.0, (double) (drc * makeup), 1e-4,
      "non-centre channel: drc x makeup");
  near (buf[CENTER_CH] / 1000000.0, (double) (drc * ctr * makeup), 1e-4,
      "centre channel:     drc x center x makeup");
  near (20.0 * log10 (buf[CENTER_CH] / (double) buf[0]), 3.0, 1e-3,
      "centre sits exactly +3 dB over the rest");
}

/* ---------------------------------------------------------------------------
 * 5. The inert default: no config file -> nothing is enabled, and the decoder
 *    takes the shipped make-up-gain call with an exact linear 1.0.
 * ------------------------------------------------------------------------ */
static void
test_inert_default (void)
{
  DtsDrcConfig cfg;
  int drc_active;

  puts ("[5] inert default (drc=off, center=0, gain=0)");

  dts_drc_config_read_file ("/nonexistent/webosbrew/truehd/gain.conf", &cfg);
  report (cfg.drc_mode == DTS_DRC_MODE_OFF, "no config file -> drc off");
  near (cfg.center_db, 0.0, 0.0, "no config file -> center exactly 0 dB");
  near (cfg.gain_db, 0.0, 0.0, "no config file -> gain exactly 0 dB");

  /* the same expression mlp_decode_init() computes */
  drc_active = (cfg.drc_mode != DTS_DRC_MODE_OFF || cfg.center_db != 0.0f);
  report (!drc_active,
      "-> drc_active == 0, so output_data() takes the shipped gain-only call");
  near ((double) dts_drc_db_to_linear (cfg.center_db), 1.0, 0.0,
      "   and 0 dB converts to an EXACT linear 1.0 (no pow rounding)");
}

/* ---------------------------------------------------------------------------
 * 6. NaN hardening on the TrueHD-only legacy make-up-gain path.
 *
 * This reader and this clamp are NOT part of the shared core — they are
 * deliberately double-precision so the already-shipped gain path stays
 * bit-exact — which means they needed the same NaN fix separately. A NaN gain
 * would make mlp_apply_makeup_gain() write (int32_t) NaN for every sample of
 * every channel: undefined behaviour. Reachable, because the app writes this
 * file from JavaScript and String(NaN) is the literal "NaN".
 * ------------------------------------------------------------------------ */

/* NEGATIVE CONTROL ONLY — the pre-fix clamp, so the assertions below cannot
 * pass against a reverted fix. */
static double
naive_gain_clamp (double gain_db)
{
  if (gain_db > DTS_MAKEUP_GAIN_DB_MAX)
    gain_db = DTS_MAKEUP_GAIN_DB_MAX;
  else if (gain_db < DTS_MAKEUP_GAIN_DB_MIN)
    gain_db = DTS_MAKEUP_GAIN_DB_MIN;
  return gain_db;
}

static void
write_gain_conf (const char *content)
{
  FILE *f = fopen (MLP_MAKEUP_GAIN_CONF_PATH, "w");

  if (f == NULL) {
    fprintf (stderr, "cannot write %s\n", MLP_MAKEUP_GAIN_CONF_PATH);
    exit (2);
  }
  fputs (content, f);
  fclose (f);
}

static void
test_makeup_gain_nan (void)
{
  const double nan_d = 0.0 / 0.0;
  static const char *spellings[] = { "nan\n", "NaN\n", "-NAN\n",
    "# tuned\n\n  +nan  \n"
  };
  size_t k;
  double g;

  puts ("[6] legacy make-up-gain reader — NaN hardening");

  for (k = 0; k < sizeof (spellings) / sizeof (spellings[0]); k++) {
    char label[128];

    write_gain_conf (spellings[k]);
    g = mlp_read_makeup_gain_db ();
    snprintf (label, sizeof (label), "NaN spelling %u -> 0.0 dB, not NaN",
        (unsigned) k);
    report (g == 0.0, label);
    near (mlp_makeup_gain_db_to_linear (g), 1.0, 0.0,
        "  ...and an EXACT linear 1.0, so the path stays a no-op");
  }

  /* NEGATIVE CONTROL: without the fix the same input reaches pow() as NaN. */
  {
    double bad = pow (10.0, naive_gain_clamp (nan_d) / 20.0);

    report (bad != bad,
        "negative control: the pre-fix reader/clamp yields a NaN multiplier");
    report (mlp_makeup_gain_db_to_linear (nan_d) ==
        pow (10.0, DTS_MAKEUP_GAIN_DB_MIN / 20.0),
        "the hardened clamp turns a NaN dB into the -20 dB floor instead");
  }

  /* The shipped behaviour must be untouched for every ordinary value: the
   * legacy bare float still wins, still in double, still clamped the same. */
  write_gain_conf ("6.0\n");
  g = mlp_read_makeup_gain_db ();
  report (g == 6.0, "a normal bare float is still read exactly");
  report (mlp_makeup_gain_db_to_linear (g) == pow (10.0, 6.0 / 20.0),
      "  ...and converts through the DOUBLE pow(), bit-for-bit as shipped");
  write_gain_conf ("999\n");
  report (mlp_read_makeup_gain_db () == 999.0
      && mlp_makeup_gain_db_to_linear (999.0)
      == pow (10.0, DTS_MAKEUP_GAIN_DB_MAX / 20.0),
      "out-of-range gain still clamps to +20 dB (amendment B)");
  write_gain_conf ("inf\n");
  report (mlp_makeup_gain_db_to_linear (mlp_read_makeup_gain_db ())
      == pow (10.0, DTS_MAKEUP_GAIN_DB_MAX / 20.0),
      "inf still clamps to +20 dB — only NaN changed behaviour");
  write_gain_conf ("not a number\n");
  report (mlp_read_makeup_gain_db () == 0.0, "junk -> 0.0 dB unity");
  remove (MLP_MAKEUP_GAIN_CONF_PATH);
  report (mlp_read_makeup_gain_db () == 0.0, "missing file -> 0.0 dB unity");
}

int
main (void)
{
  /* Core functions this file does not exercise (run-tests.sh's 146 assertions
   * cover them); referenced so -Werror=unused-function does not fire on the
   * verbatim core. */
  (void) dts_drc_mode_to_string;
  (void) dts_drc_scale_gain_db;
  (void) dts_drc_smooth_coef;
  (void) dts_drc_smooth_step;
  (void) dts_drc_config_read_file;
  (void) mlp_apply_makeup_gain;

  puts ("=== webOS 25 mlpdec DRC host-binding test ===");
  test_detector_parity ();
  test_lfe_exclusion ();
  test_apply ();
  test_order ();
  test_inert_default ();
  test_makeup_gain_nan ();
  printf ("\n=== %d checks, %d failures ===\n", checks, failures);
  return failures ? 1 : 0;
}
