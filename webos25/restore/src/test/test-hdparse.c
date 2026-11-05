/* Host test for ../hdparse.h, the frame delimiting hdbitstream feeds its
 * packers with. The DTS-HD fixture (iec61937-dtshd.in, 3 core+EXSS frames)
 * and the TrueHD one (iec61937-truehd.in, 77 access units) are the packer
 * test's inputs, so what this splits is exactly what the packer was proven
 * on. The split points are checked against the sync words themselves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdparse.h"

static int failures;

static void
check (int ok, const char *what)
{
  if (!ok) {
    fprintf (stderr, "FAIL: %s\n", what);
    failures++;
  }
}

static uint8_t *
slurp (const char *dir, const char *name, long *n)
{
  char path[1024];
  FILE *f;
  uint8_t *d;

  snprintf (path, sizeof path, "%s/%s", dir, name);
  f = fopen (path, "rb");
  if (!f) {
    perror (path);
    exit (2);
  }
  fseek (f, 0, SEEK_END);
  *n = ftell (f);
  rewind (f);
  d = malloc (*n + 1);
  if (!d || fread (d, 1, *n, f) != (size_t) *n) {
    perror (path);
    exit (2);
  }
  fclose (f);
  return d;
}

static void
test_dts (const char *dir)
{
  long n, off = 0;
  uint8_t *d = slurp (dir, "iec61937-dtshd.in", &n);
  int frames = 0, size;

  while (off < n) {
    size = hdparse_dts_frame (d + off, (int) (n - off));
    check (size > 0, "DTS: a frame starts at every frame boundary");
    if (size <= 0)
      break;
    /* core, then an EXSS, never a bare core for this fixture */
    check (hdparse_dts_core (d + off, size) > 0
        && hdparse_dts_core (d + off, size) < size, "DTS: core + EXSS");
    check (hdparse_be32 (d + off + hdparse_dts_core (d + off, size))
        == HDPARSE_DTS_SYNC_EXSS, "DTS: EXSS follows the core");
    off += size;
    frames++;
    /* FFmpeg's dca parser cut this fixture; it keeps one stray byte before
     * the third frame, which the scan in hdbitstream's parse skips */
    if (off < n && hdparse_be32 (d + off) != HDPARSE_DTS_SYNC_CORE) {
      check (hdparse_dts_frame (d + off, (int) (n - off)) == 0,
          "DTS: a stray byte is not a frame");
      off++;
    }
  }
  check (off == n && frames == 3, "DTS: 3 frames cover the fixture");

  /* truncated: more data needed, never a short frame */
  check (hdparse_dts_frame (d, 4) == -1, "DTS: 4 bytes need more");
  check (hdparse_dts_frame (d, hdparse_dts_core (d, 16) + 2) == -1,
      "DTS: core without the EXSS header yet needs more");
  /* an EXSS alone is a frame (DTS Express opens with these) */
  off = hdparse_dts_core (d, 16);
  check (hdparse_dts_frame (d + off, (int) (n - off)) > 0
      && hdparse_dts_frame (d + off, (int) (n - off))
      == hdparse_dts_exss (d + off, (int) (n - off)), "DTS: EXSS alone");
  /* not a frame start: scanned past */
  check (hdparse_dts_frame (d + 1, (int) (n - 1)) == 0, "DTS: offset 1 is nothing");
  d[0] ^= 0x80;
  check (hdparse_dts_frame (d, (int) n) == 0, "DTS: broken sync is nothing");
  free (d);
}

static void
test_truehd (const char *dir)
{
  long n, off = 0;
  uint8_t *d = slurp (dir, "iec61937-truehd.in", &n);
  int aus = 0, syncs = 0, size;

  /* the first AU carries a major sync; every later one is taken on its
   * length word alone, as the stream has no sync between them */
  check (hdparse_mlp_au (d, (int) n, 1) > 0, "TrueHD: starts with a major sync");
  while (off < n) {
    size = hdparse_mlp_au (d + off, (int) (n - off), aus == 0);
    check (size > 0, "TrueHD: an AU starts at every AU boundary");
    if (size <= 0)
      break;
    if (hdparse_mlp_has_sync (d + off, size))
      syncs++;
    off += size;
    aus++;
  }
  check (off == n && aus == 77, "TrueHD: 77 AUs cover the fixture");
  check (syncs == 5, "TrueHD: 5 of them carry a major sync");

  /* with a sync required, an AU without one is nothing */
  size = hdparse_mlp_au (d, (int) n, 1);
  check (hdparse_mlp_au (d + size, (int) (n - size), 1) == 0,
      "TrueHD: second AU has no sync");
  check (hdparse_mlp_au (d + size, (int) (n - size), 0) > 0,
      "TrueHD: but its length is trusted after a frame");
  check (hdparse_mlp_au (d, 7, 1) == -1, "TrueHD: 7 bytes need more");
  /* an impossible length word is nothing, sync or not */
  d[0] = 0;
  d[1] = 1;
  check (hdparse_mlp_au (d, (int) n, 0) == 0, "TrueHD: 2-byte AU is nothing");
  /* 8 bytes: too short for the MAT packer, so not a frame here either */
  d[1] = 4;
  check (hdparse_mlp_au (d, (int) n, 0) == 0, "TrueHD: 8-byte AU is nothing");
  d[1] = 5;
  check (hdparse_mlp_au (d, (int) n, 0) == 10, "TrueHD: 10-byte AU is one");
  free (d);
}

int
main (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "usage: %s <dir with iec61937-*.in>\n", argv[0]);
    return 2;
  }
  test_dts (argv[1]);
  test_truehd (argv[1]);
  if (failures)
    return 1;
  printf ("ok - hdparse splits the DTS-HD and TrueHD fixtures at their frames\n");
  return 0;
}
