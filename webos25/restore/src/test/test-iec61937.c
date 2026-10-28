/* Host test for dts_iec61937_pack(): two frames of the bundled DTS-HD MA
 * sample (core + extension) must come out byte for byte as
 * `ffmpeg -c:a copy -f spdif` writes them (iec61937-dts.ref). The function is
 * pulled out of ../gstdtsdec.c by run-tests.sh, so this tests shipped code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int gint;
typedef unsigned int guint;
typedef unsigned char guint8;
typedef uint32_t guint32;
typedef uint64_t guint64;
typedef size_t gsize;
typedef int gboolean;
#define GST_READ_UINT32_BE(p) ((guint32) (p)[0] << 24 | (guint32) (p)[1] << 16 | (guint32) (p)[2] << 8 | (p)[3])
#define GST_READ_UINT24_BE(p) ((guint32) (p)[0] << 16 | (guint32) (p)[1] << 8 | (p)[2])
#define GST_READ_UINT16_BE(p) ((guint) (p)[0] << 8 | (p)[1])
#define GST_WRITE_UINT16_LE(p, v) do { (p)[0] = (v) & 0xff; (p)[1] = ((v) >> 8) & 0xff; } while (0)
#include "iec61937.inc"

static guint8 *
slurp (const char *path, long *n)
{
  FILE *f = fopen (path, "rb");
  guint8 *d;

  if (!f) { perror (path); exit (1); }
  fseek (f, 0, SEEK_END);
  *n = ftell (f);
  rewind (f);
  d = malloc (*n);
  if (fread (d, 1, *n, f) != (size_t) *n) { perror (path); exit (1); }
  fclose (f);
  return d;
}

int
main (int argc, char **argv)
{
  long n_in, n_ref, off = 0, pos = 0;
  guint8 *in, *ref, out[8192];
  char p[1024];

  if (argc != 2) {
    fprintf (stderr, "usage: %s <dir with iec61937-dts.in/.ref>\n", argv[0]);
    return 2;
  }
  snprintf (p, sizeof p, "%s/iec61937-dts.in", argv[1]);
  in = slurp (p, &n_in);
  snprintf (p, sizeof p, "%s/iec61937-dts.ref", argv[1]);
  ref = slurp (p, &n_ref);

  while (off < n_in) {
    gint len = dts_frame_size (in + off, (gint) (n_in - off));
    gint period = len > 0 ? dts_iec61937_pack (in + off, len, NULL, 0) : 0;

    if (period == 0 || dts_iec61937_pack (in + off, len, out, period) != period
        || pos + period > n_ref || memcmp (out, ref + pos, period) != 0) {
      fprintf (stderr, "FAIL: burst at input %ld differs from ffmpeg\n", off);
      return 1;
    }
    off += len;
    pos += period;
  }
  if (pos != n_ref) {
    fprintf (stderr, "FAIL: %ld of %ld reference bytes produced\n", pos, n_ref);
    return 1;
  }
  /* not a 16-bit big-endian 48 kHz core: refused, so the caller decodes */
  in[0] ^= 0xff;
  if (dts_iec61937_pack (in, n_in, NULL, 0) != 0) {
    fprintf (stderr, "FAIL: a frame without a core sync was accepted\n");
    return 1;
  }
  printf ("ok - IEC 61937 bursts match ffmpeg's spdif muxer (%ld bytes)\n", pos);
  return 0;
}
