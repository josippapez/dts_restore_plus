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

/* Edge lines of /proc/aaudio/dot_module_connection as the C5 printed them on
 * 2026-11-06 while hdbitstream played (DTS-18): one PCM bypass head
 * rts_04250001 -> srcbin_04260005 -> lipsync_04230006 next to the main
 * heads (srcbin -> sola -> lipsync) and the dotted shared_buf lines. */
static const char graph_playing[] =
    " mixerbin_04270001_000000002126aa69_out2 -> gluepw_04110003_000000007c055c89_in0;\n"
    " gluepw_04110003_out -> shared_buf_00000000b991b46a [style=dotted];\n"
    " shared_buf_00000000b991b46a -> gluepr_03100003_in [style=dotted];\n"
    " srcbin_04260005_00000000cad08855_out0 -> lipsync_04230006_000000008029dfde_in0;\n"
    " srcbin_04260004_000000007b4920b9_out0 -> sola_04240004_0000000018c9ecb7_in0;\n"
    " srcbin_04260003_00000000fd259866_out0 -> sola_04240003_000000005c912f1b_in0;\n"
    " rts_04250001_00000000478fec6f_out0 -> srcbin_04260005_00000000fde56590_in0;\n"
    " sola_04240004_00000000e96c700e_out0 -> lipsync_04230005_00000000727f2cde_in0;\n"
    " queue_04180002_00000000d31ae65c_out0 -> srcbin_04260004_000000004b7df91b_in0;\n"
    " queue_04180001_000000001a94edb2_out0 -> lipsync_04230004_0000000099749e11_in0;\n"
    " gluepr_04100004_000000001f5f0d76_out0 -> rts_04250001_0000000097791f12_in0;\n"
    " input_queue_02170001_00000000af7ff06a_out0 -> decoderbin_023c0001_0000000068b0aa94_in0;\n";

/* The same TV idle: the head is gone with the renderer. */
static const char graph_idle[] =
    " srcbin_04260003_00000000fd259866_out0 -> sola_04240003_000000005c912f1b_in0;\n"
    " sola_04240003_000000000c695c62_out0 -> lipsync_04230003_00000000d4c294e2_in0;\n"
    " queue_04180001_000000001a94edb2_out0 -> lipsync_04230004_0000000099749e11_in0;\n";

/* A no-delay main head (rts prepended, renderer.c:1499) must not count: its
 * srcbin feeds sola, not lipsync. */
static const char graph_nodelay_main[] =
    " rts_04250002_00000000478fec6f_out0 -> srcbin_04260004_000000004b7df91b_in0;\n"
    " srcbin_04260004_000000007b4920b9_out0 -> sola_04240004_0000000018c9ecb7_in0;\n"
    " rts_04250001_00000000478fec6f_out0 -> srcbin_04260005_00000000fde56590_in0;\n"
    " srcbin_04260005_00000000cad08855_out0 -> lipsync_04230006_000000008029dfde_in0;\n";

static const char graph_two_heads[] =
    " rts_04250001_00000000478fec6f_out0 -> srcbin_04260005_00000000fde56590_in0;\n"
    " srcbin_04260005_00000000cad08855_out0 -> lipsync_04230006_000000008029dfde_in0;\n"
    " rts_04250002_00000000478fec6f_out0 -> srcbin_04260007_00000000fde56590_in0;\n"
    " srcbin_04260007_00000000cad08855_out0 -> lipsync_04230008_000000008029dfde_in0;\n";

static void
test_graph (void)
{
  char uid[9] = "", impl[32], port[16];
  int chains = -1;

  check (lgroute_graph_node ("rts_04250001_00000000478fec6f_out0", impl,
          sizeof (impl), uid, port, sizeof (port))
      && !strcmp (impl, "rts") && !strcmp (uid, "04250001")
      && !strcmp (port, "out0"), "node with a plain impl name");
  check (lgroute_graph_node ("input_queue_02170001_00000000af7ff06a_out0", impl,
          sizeof (impl), uid, port, sizeof (port))
      && !strcmp (impl, "input_queue") && !strcmp (uid, "02170001"),
      "node whose impl name has an underscore");
  check (!lgroute_graph_node ("gluepw_04110003_out", impl, sizeof (impl), uid,
          port, sizeof (port)), "a shared_buf endpoint has no hash");
  check (!lgroute_graph_node ("shared_buf_00000000b991b46a", impl,
          sizeof (impl), uid, port, sizeof (port)), "a shared_buf node");

  uid[0] = '\0';
  check (lgroute_find_bypass_srcbin (graph_playing, uid, &chains)
      && chains == 1 && !strcmp (uid, "04260005"),
      "the C5's playing graph names srcbin_04260005");
  check (!lgroute_find_bypass_srcbin (graph_idle, uid, &chains) && chains == 0,
      "the idle graph has no head");
  check (lgroute_find_bypass_srcbin (graph_nodelay_main, uid, &chains)
      && chains == 1 && !strcmp (uid, "04260005"),
      "a no-delay main head's rts -> srcbin -> sola does not count");
  check (!lgroute_find_bypass_srcbin (graph_two_heads, uid, &chains)
      && chains == 2, "two heads: refused");
  check (!lgroute_find_bypass_srcbin ("", uid, &chains) && chains == 0,
      "empty graph");
}

static void
test_format (void)
{
  LgRouteFmt fmt = LGROUTE_FMT_NONE;
  char cmd[96];

  check (lgroute_marker_fmt ("force\n", &fmt) && fmt == LGROUTE_FMT_OUT16,
      "no fmt token: default is out 16");
  check (lgroute_marker_fmt (NULL, &fmt) && fmt == LGROUTE_FMT_OUT16,
      "no marker content: default");
  check (lgroute_marker_fmt ("force fmt=none", &fmt) && fmt == LGROUTE_FMT_NONE,
      "fmt=none");
  check (lgroute_marker_fmt ("fmt=16\nforce", &fmt) && fmt == LGROUTE_FMT_OUT16,
      "fmt=16");
  check (lgroute_marker_fmt ("fmt=32", &fmt) && fmt == LGROUTE_FMT_OUT32,
      "fmt=32");
  check (!lgroute_marker_fmt ("fmt=24", &fmt) && fmt == LGROUTE_FMT_OUT16,
      "fmt=24: not understood, default kept");
  check (lgroute_marker_fmt ("fmt=none", &fmt) && fmt == LGROUTE_FMT_NONE, "none again");
  check (!lgroute_format_command (LGROUTE_FMT_NONE, cmd, sizeof (cmd)),
      "fmt=none writes nothing");
  check (lgroute_format_command (LGROUTE_FMT_OUT16, cmd, sizeof (cmd))
      && !strcmp (cmd, "0x50020020 192000 8 16 0 0 192000 8 16\n"),
      "out 16: in == out, the no-op request");
  check (lgroute_format_command (LGROUTE_FMT_OUT32, cmd, sizeof (cmd))
      && !strcmp (cmd, "0x50020020 192000 8 16 0 0 192000 8 32\n"),
      "out 32: Glasshouse's line (c5-route.c:137)");
}

int
main (void)
{
  test_flip ();
  test_sound_output ();
  test_carrier ();
  test_graph ();
  test_format ();
  if (failures) {
    fprintf (stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf ("lgroute policy: all checks passed\n");
  return 0;
}
