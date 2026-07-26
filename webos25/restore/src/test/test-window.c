/* F1 regression test: does the MLP detector settle on the SAME gain as the DTS
 * detector for identical audio?
 *
 * The DTS path measures a fixed 256-sample libdca block. An MLP access unit is
 * only 40 samples, so measuring per access unit shrinks the averaging window
 * 6.4x — and because the curve is nonlinear and unimodal and the smoother is
 * asymmetric (fast attack / slow release), the smoother then tracks the peaks
 * of a noisier level estimate and SETTLES SOMEWHERE ELSE. This file measures
 * that, three ways, driving the real shipped core:
 *
 *   dts   : 256-sample blocks                      (the reference)
 *   au    : 40-sample access units, stepped each   (the REJECTED design)
 *   accum : 40-sample access units accumulated to  (the SHIPPED design)
 *           >= 256 before stepping
 *
 * `accum` must track `dts`; `au` is kept as a negative control so the test
 * fails loudly if someone "simplifies" the accumulator away.
 *
 * Tolerance is 0.45 dB, not the 0.30 dB first used here: the agreement is
 * signal-dependent, and an independent reviewer's signal set reached 0.358 dB
 * where this file's reaches 0.16 dB. 0.30 would have been flaky. The rejected
 * per-AU design misses by 1.5-3.9 dB, so the two are never close to confusable.
 *
 * Caveat, stated plainly: the accumulate-and-step POLICY lives inline in
 * output_data() and cannot be extracted, so it is modelled here (~10 lines).
 * The curve, detector, scaling and smoother are the real extracted core.
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

#define MAX_CHANNELS 8
#include "drc-core.inc"
#include "mlp-binding.inc"

#define RATE 48000
#define NCH 6
#define LFE_CH 3
#define DUR_S 4
#define NTOTAL (RATE * DUR_S)

static int checks = 0, failures = 0;

static float *sig;              /* mono source, +-1.0 */

/* One-pole step at a given window length, driving the real core. */
static float
run (int window, int mode, int accumulate)
{
  float smoothed = 0.0f;
  float acc_sum = 0.0f;
  int acc_count = 0, acc_samples = 0;
  float period = (float) window / (float) RATE;
  const DtsDrcProfile *p = &dts_drc_profiles[mode];
  float atk = dts_drc_smooth_coef (p->attack_ms, period);
  float rel = dts_drc_smooth_coef (p->release_ms, period);
  int step = accumulate ? 40 : window;   /* chunk actually delivered */
  int i;

  /* When accumulating, the coefficient belongs to the ACCUMULATED window, and
   * 40-sample units reach 256 only at 280 — exactly what the port does. */
  if (accumulate) {
    int w = ((256 + 39) / 40) * 40;

    period = (float) w / (float) RATE;
    atk = dts_drc_smooth_coef (p->attack_ms, period);
    rel = dts_drc_smooth_coef (p->release_ms, period);
  }

  for (i = 0; i + step <= NTOTAL; i += step) {
    int n, c, count = 0;
    float sum = 0.0f;

    /* detector over the chunk, LFE excluded (LFE is silent here) */
    for (c = 0; c < NCH; c++) {
      if (c == LFE_CH)
        continue;
      for (n = 0; n < step; n++)
        sum += sig[i + n] * sig[i + n];
      count += step;
    }

    acc_sum += sum;
    acc_count += count;
    acc_samples += step;

    if (!accumulate || acc_samples >= DTS_DRC_BLOCK_SAMPLES) {
      float level_db = dts_drc_level_dbfs (acc_sum, acc_count);
      float target_db = dts_drc_scale_gain_db
          (dts_drc_target_gain_db (mode, level_db), 100.0f, 100.0f);

      smoothed = dts_drc_smooth_step (smoothed, target_db, atk, rel);
      acc_sum = 0.0f;
      acc_count = 0;
      acc_samples = 0;
    }
  }
  return smoothed;
}

static void
compare (const char *what, int mode, double tol)
{
  float dts = run (256, mode, 0);
  float au = run (40, mode, 0);
  float accum = run (40, mode, 1);
  double d_accum = fabs (accum - dts);
  double d_au = fabs (au - dts);
  const char *mname = dts_drc_mode_to_string (mode);
  int ok = d_accum <= tol;

  checks++;
  if (!ok)
    failures++;
  printf ("  %-4s %-28s %-4s  dts %+7.3f | accum %+7.3f (d %.3f) | "
      "per-AU %+7.3f (d %.3f)\n", ok ? "ok" : "FAIL", what, mname,
      dts, accum, d_accum, au, d_au);

  /* Negative control: the rejected per-AU design must be measurably worse on
   * the LF/dialogue-band cases, otherwise this test proves nothing. */
  if (d_au > 0.5) {
    checks++;
    if (!(d_au > d_accum * 2.0)) {
      failures++;
      printf ("  FAIL   negative control: per-AU should be far worse here\n");
    } else {
      printf ("  ok     negative control: per-AU is %.2f dB off, accum %.3f dB\n",
          d_au, d_accum);
    }
  }
}

static void
mk_sine (double hz, double rms_dbfs)
{
  double amp = pow (10.0, rms_dbfs / 20.0) * sqrt (2.0);
  int i;

  for (i = 0; i < NTOTAL; i++)
    sig[i] = (float) (amp * sin (2.0 * M_PI * hz * i / RATE));
}

static void
mk_noise (double rms_dbfs, int lowpass)
{
  double amp = pow (10.0, rms_dbfs / 20.0);
  double y = 0.0;
  unsigned s = 12345;
  int i;

  for (i = 0; i < NTOTAL; i++) {
    double x;

    s = s * 1103515245u + 12345u;
    x = ((double) ((s >> 9) & 0x7fffff) / 4194304.0) - 1.0;
    if (lowpass) {
      y += 0.02 * (x - y);      /* ~150 Hz one-pole -> LF-weighted */
      x = y * 7.0;
    }
    sig[i] = (float) (amp * x);
  }
}

int
main (void)
{
  (void) dts_drc_config_read_file;
  (void) dts_drc_db_to_linear;
  (void) dts_drc_sum_squares;
  (void) mlp_drc_sum_squares_ilv;
  (void) mlp_drc_apply;
  (void) mlp_read_makeup_gain_db;
  (void) mlp_makeup_gain_db_to_linear;
  (void) mlp_apply_makeup_gain;

  sig = malloc (sizeof (float) * NTOTAL);
  if (!sig)
    return 2;

  puts ("=== F1: MLP detector window vs the DTS reference ===");
  puts ("    (accum = shipped design; per-AU = rejected design, kept as control)");

  mk_sine (180.0, -46.0);
  compare ("180 Hz speech-band -46", DTS_DRC_MODE_LINE, 0.45);
  compare ("180 Hz speech-band -46", DTS_DRC_MODE_RF, 0.45);

  mk_sine (100.0, -60.0);
  compare ("100 Hz -60", DTS_DRC_MODE_LINE, 0.45);
  compare ("100 Hz -60", DTS_DRC_MODE_RF, 0.45);

  mk_sine (50.0, -50.0);
  compare ("50 Hz -50", DTS_DRC_MODE_LINE, 0.45);

  mk_noise (-46.0, 1);
  compare ("LF-weighted noise -46", DTS_DRC_MODE_LINE, 0.45);
  mk_noise (-56.0, 1);
  compare ("LF-weighted noise -56", DTS_DRC_MODE_RF, 0.45);

  mk_noise (-46.0, 0);
  compare ("white noise -46", DTS_DRC_MODE_LINE, 0.45);
  mk_noise (-60.0, 0);
  compare ("white noise -60", DTS_DRC_MODE_RF, 0.45);

  free (sig);
  printf ("\n=== %d checks, %d failures ===\n", checks, failures);
  return failures ? 1 : 0;
}
