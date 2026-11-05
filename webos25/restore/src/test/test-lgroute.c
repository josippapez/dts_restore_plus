/* Host test for ../lgroute-policy.h: the pcm-multi-ch flip that LG's eARC
 * route depends on (TRUE while the carrier CAPS pass lgaudiosink, FALSE
 * before its first buffer, nothing on repeats) and the opt-in decision read
 * from the watcher's sound-output line and the marker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lgroute-policy.h"

static int failures;

static void
check (int ok, const char *what)
{
  if (!ok) {
    fprintf (stderr, "FAIL: %s\n", what);
    failures++;
  }
}

static void
test_flip (void)
{
  LgRouteFlip f;

  lgroute_flip_init (&f);
  check (lgroute_flip_step (&f, LGROUTE_IN_BUFFER) == LGROUTE_ACT_NONE,
      "a buffer before our caps changes nothing");
  check (lgroute_flip_step (&f, LGROUTE_IN_OTHER_CAPS) == LGROUTE_ACT_NONE,
      "someone else's caps before ours change nothing");
  check (lgroute_flip_step (&f, LGROUTE_IN_CARRIER_CAPS) == LGROUTE_ACT_SET_TRUE,
      "the carrier caps set the flag");
  check (f.flag == 1, "flag recorded as TRUE");
  check (lgroute_flip_step (&f, LGROUTE_IN_CARRIER_CAPS) == LGROUTE_ACT_NONE,
      "the same caps again do not set it twice");
  check (lgroute_flip_step (&f, LGROUTE_IN_BUFFER) == LGROUTE_ACT_SET_FALSE,
      "the first buffer clears the flag");
  check (f.flag == 0 && f.flips == 1, "one flip counted");
  check (lgroute_flip_step (&f, LGROUTE_IN_BUFFER) == LGROUTE_ACT_NONE,
      "later buffers change nothing");

  /* sticky caps re-pushed after a relink: set again, cleared again */
  check (lgroute_flip_step (&f, LGROUTE_IN_CARRIER_CAPS) == LGROUTE_ACT_SET_TRUE,
      "caps re-pushed set the flag again");
  check (lgroute_flip_step (&f, LGROUTE_IN_BUFFER) == LGROUTE_ACT_SET_FALSE,
      "and the next buffer clears it again");
  check (f.flips == 2, "two flips counted");

  /* another decoder's caps while the flag is up: cleared, not counted */
  check (lgroute_flip_step (&f, LGROUTE_IN_CARRIER_CAPS) == LGROUTE_ACT_SET_TRUE,
      "set once more");
  check (lgroute_flip_step (&f, LGROUTE_IN_OTHER_CAPS) == LGROUTE_ACT_SET_FALSE,
      "foreign caps clear a flag we left up");
  check (f.flips == 2, "a clear on foreign caps is not a flip");
}

static void
test_sound_output (void)
{
  int pid = 0;

  check (lgroute_parse_sound_output ("1234 external_arc passThrough\n", &pid)
      && pid == 1234, "receiver with Pass Through");
  check (lgroute_parse_sound_output ("77 external_arc auto", &pid) && pid == 77,
      "receiver with Auto");
  check (!lgroute_parse_sound_output ("77 external_arc pcm", &pid),
      "receiver with PCM is not wanted");
  check (!lgroute_parse_sound_output ("77 tv_speaker passThrough", &pid),
      "speakers are not wanted");
  check (!lgroute_parse_sound_output ("77 external_arc", &pid),
      "a line without the digital mode is not wanted");
  check (!lgroute_parse_sound_output ("0 external_arc auto", &pid),
      "a dead watcher pid is not wanted");
  check (!lgroute_parse_sound_output ("", &pid), "empty file");
  check (!lgroute_parse_sound_output (NULL, &pid), "no file");

  check (lgroute_marker_forces ("force\n"), "marker 'force' skips the check");
  check (!lgroute_marker_forces (""), "an empty marker does not");
  check (!lgroute_marker_forces (NULL), "no marker content does not");
}

static void
test_carrier (void)
{
  /* The values the decomp pins (see the header): a change here is a change
   * to what lgaudiosink sends the kernel and dsp1. */
  check (LGROUTE_CARRIER_RATE == 192000 && LGROUTE_CARRIER_CHANNELS == 8,
      "8 ch / 192 kHz carrier = 4 x 48 kHz stereo HBR lanes");
  check (LGROUTE_CARRIER_DATA_RATE == 4 * 192000, "data-rate 768000 selects HBR");
  check (LGROUTE_CARRIER_DATA_CH == 8 && LGROUTE_CARRIER_DATA_BPS == 16,
      "8 x 16-bit ES words per frame: the cpy_es8 lane writer");
  check (LGROUTE_CARRIER_ACMOD == 0, "acmod 0 so a fresh sink sees no acmod change");
  check (strcmp (LGROUTE_CARRIER_CODEC_STR, "mat") == 0,
      "codec-str mat: the only HBR codec the kernel keeps as non-PCM");
  check (LGROUTE_BUFFER_FLAG_ES == (1u << 21), "LG's ES buffer flag is bit 21");
}

int
main (void)
{
  test_flip ();
  test_sound_output ();
  test_carrier ();
  if (failures) {
    fprintf (stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf ("lgroute policy: all checks passed\n");
  return 0;
}
