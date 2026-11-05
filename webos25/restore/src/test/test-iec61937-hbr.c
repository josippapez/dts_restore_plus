/* Host test for ../iec61937-hbr.c: DTS-HD (type 17) bursts for the 768 kHz
 * carrier and TrueHD MAT (type 22) bursts must come out byte for byte as
 * FFmpeg 9.0.2's spdif muxer writes them:
 *
 *   iec61937-dtshd.ref   ffmpeg -f dts -i iec61937-dtshd.in -c:a copy \
 *                          -f spdif -dtshd_rate 768000 iec61937-dtshd.ref
 *   iec61937-truehd.ref  ffmpeg -f truehd -i iec61937-truehd.in -c:a copy \
 *                          -f spdif iec61937-truehd.ref
 *
 * (iec61937-truehd96.* likewise). make-iec61937-hbr-fixtures.py makes them
 * all and says what is in them: 3 DTS-HD MA frames, one of them odd-sized;
 * 77 TrueHD access units edited so every branch of FFmpeg's MAT padding runs;
 * 30 of them as a 96 kHz stream. The input is cut into the same packets
 * FFmpeg's parsers cut. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iec61937-hbr.h"

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
  d = malloc (*n);
  if (!d || fread (d, 1, *n, f) != (size_t) * n) {
    perror (path);
    exit (2);
  }
  fclose (f);
  return d;
}

/* A DTS packet runs from one core sync word to the next, as FFmpeg's dca
 * parser cut these files (it keeps a stray byte before the next sync). */
static long
dts_packet (const uint8_t * d, long avail)
{
  long i;

  for (i = 4; i + 4 <= avail; i++)
    if (d[i] == 0x7f && d[i + 1] == 0xfe && d[i + 2] == 0x80
        && d[i + 3] == 0x01)
      return i;
  return avail;
}

/* NBLKS, the 7-bit block count - 1: bit 0 of byte 4, bits 7..2 of byte 5 */
static void
set_nblks (uint8_t * d, unsigned v)
{
  d[4] = (d[4] & 0xfe) | (v >> 6);
  d[5] = (d[5] & 0x03) | (v & 0x3f) << 2;
}

static void
test_dtshd (const char *dir)
{
  static uint8_t out[IEC61937_DTSHD_MAX_BURST], big[32750];
  long n_in, n_ref, off = 0, pos = 0;
  uint8_t *in = slurp (dir, "iec61937-dtshd.in", &n_in);
  uint8_t *ref = slurp (dir, "iec61937-dtshd.ref", &n_ref);
  int bursts = 0, len, i;

  while (off < n_in) {
    long size = dts_packet (in + off, n_in - off);

    len = iec61937_dtshd_pack (in + off, size, out, sizeof out);
    if (len <= 0 || iec61937_dtshd_pack (in + off, size, NULL, 0) != len
        || pos + len > n_ref || memcmp (out, ref + pos, len) != 0) {
      fprintf (stderr, "FAIL: DTS-HD burst for the %ld-byte packet at input "
          "%ld differs from ffmpeg (returned %d)\n", size, off, len);
      exit (1);
    }
    check (len == 32768 && IEC61937_HBR_FRAMES (len) == 2048,
        "48 kHz 512-sample DTS-HD burst is 32768 bytes / 2048 frames");
    off += size;
    pos += len;
    bursts++;
  }
  check (pos == n_ref, "every DTS-HD reference byte produced");

  /* The largest frame that fits fills the burst with no padding; one more
   * byte is refused, and refusing writes nothing. */
  memcpy (big, in, 2568);
  for (i = 2568; i < (int) sizeof big; i++)
    big[i] = (uint8_t) (i * 13 + 5);
  check (iec61937_dtshd_pack (big, 32748, out, sizeof out) == 32768
      && out[6] == (32760 & 0xff) && out[7] == (32760 >> 8)
      && out[32766] == big[32747] && out[32767] == big[32746],
      "a 32748-byte frame fills the 32768-byte burst exactly");
  memset (out, 0x5a, sizeof out);
  check (iec61937_dtshd_pack (big, 32749, out, sizeof out)
      == IEC61937_HBR_E2BIG && iec61937_dtshd_pack (big, 32749, NULL, 0)
      == IEC61937_HBR_E2BIG && out[0] == 0x5a && out[32767] == 0x5a,
      "a 32749-byte frame is refused, nothing written");
  check (iec61937_dtshd_pack (in, 2568, out, 32767) == IEC61937_HBR_ENOSPC,
      "an out buffer one byte short is refused");

  /* the period follows the frame length; 2048 samples and 44.1 kHz have none */
  memcpy (big, in, 2568);
  set_nblks (big, 31);                  /* 32 blocks of 32: 1024 samples */
  check (iec61937_dtshd_pack (big, 2568, out, sizeof out) == 65536
      && out[4] == IEC61937_TYPE_DTSHD && out[5] == 0x05,
      "1024-sample frame: 65536-byte burst, subtype 5");
  set_nblks (big, 63);                  /* 2048 samples */
  check (iec61937_dtshd_pack (big, 2568, NULL, 0) == IEC61937_HBR_ERATE,
      "2048-sample frame has no 768 kHz period");
  memcpy (big, in, 2568);
  big[8] = (big[8] & 0xc3) | 8 << 2;    /* SFREQ 8: 44.1 kHz */
  check (iec61937_dtshd_pack (big, 2568, NULL, 0) == IEC61937_HBR_ERATE,
      "44.1 kHz DTS has no 768 kHz period");
  big[8] &= 0xc3;                       /* SFREQ 0: invalid */
  check (iec61937_dtshd_pack (big, 2568, NULL, 0) == IEC61937_HBR_EINVAL,
      "an invalid sample rate code is refused");

  /* not a 16-bit big-endian core: an extension-only frame, or too short */
  memcpy (big, "\x64\x58\x20\x25", 4);
  check (iec61937_dtshd_pack (big, 2568, NULL, 0) == IEC61937_HBR_EINVAL,
      "an extension substream without a core is refused");
  check (iec61937_dtshd_pack (in, 8, NULL, 0) == IEC61937_HBR_EINVAL,
      "an 8-byte frame is refused");

  printf ("ok   DTS-HD: %d bursts match ffmpeg -dtshd_rate 768000 (%ld bytes)"
      "\n", bursts, pos);
  free (in);
  free (ref);
}

static long
au_size (const uint8_t * d)
{
  return ((d[0] << 8 | d[1]) & 0xfff) * 2;
}

/* Feeds <name>.in to mat one AU at a time; every burst must be the next one
 * of <name>.ref, and all of it must come out. */
static void
mat_run (Iec61937MatPacker * mat, const char *dir, const char *name)
{
  static uint8_t out[IEC61937_MAT_BURST_BYTES];
  char file[256];
  long n_in, n_ref, off = 0, pos = 0;
  uint8_t *in, *ref;
  int aus = 0, ret;

  snprintf (file, sizeof file, "%s.in", name);
  in = slurp (dir, file, &n_in);
  snprintf (file, sizeof file, "%s.ref", name);
  ref = slurp (dir, file, &n_ref);

  while (off < n_in) {
    long size = au_size (in + off);

    if (size == 0 || off + size > n_in) {
      fprintf (stderr, "FAIL: %s: bad TrueHD AU length at input %ld\n",
          name, off);
      exit (1);
    }
    ret = iec61937_mat_push (mat, in + off, size, out, sizeof out);
    if (ret < 0 || (ret == 1 && (pos + IEC61937_MAT_BURST_BYTES > n_ref
                || memcmp (out, ref + pos, IEC61937_MAT_BURST_BYTES) != 0))) {
      fprintf (stderr, "FAIL: %s: AU %d (input %ld) made %s (returned %d)\n",
          name, aus, off, ret < 0 ? "an error" : "a burst unlike ffmpeg's",
          ret);
      exit (1);
    }
    if (ret == 1)
      pos += IEC61937_MAT_BURST_BYTES;
    off += size;
    aus++;
  }
  if (pos != n_ref) {
    fprintf (stderr, "FAIL: %s: %ld of %ld reference bytes produced\n",
        name, pos, n_ref);
    exit (1);
  }
  printf ("ok   TrueHD %s: %d AUs -> %ld MAT burst%s match ffmpeg\n", name,
      aus, pos / IEC61937_MAT_BURST_BYTES,
      pos == IEC61937_MAT_BURST_BYTES ? "" : "s");
  free (in);
  free (ref);
}

static void
test_truehd (const char *dir)
{
  static Iec61937MatPacker mat;
  static uint8_t out[IEC61937_MAT_BURST_BYTES], au[IEC61937_MAT_MAX_AU + 2];
  long n;
  uint8_t *in = slurp (dir, "iec61937-truehd.in", &n);
  long first = au_size (in);

  /* The stream's second AU, alone, carries no major sync: refused, and the
   * same packer must then still match ffmpeg. */
  iec61937_mat_init (&mat);
  check (iec61937_mat_push (&mat, in + first, au_size (in + first), out,
          sizeof out) == IEC61937_HBR_ENOSYNC,
      "an AU before any major sync is refused");
  mat_run (&mat, dir, "iec61937-truehd");
  /* 80-sample AUs: the rate code in the major sync sets the timing */
  iec61937_mat_init (&mat);
  mat_run (&mat, dir, "iec61937-truehd96");
  check (IEC61937_HBR_FRAMES (IEC61937_MAT_BURST_BYTES) == 3840,
      "a MAT burst is 3840 carrier frames");

  memset (au, 0, sizeof au);
  memcpy (au, in, first);
  check (iec61937_mat_push (&mat, au, first, out, sizeof out - 1)
      == IEC61937_HBR_ENOSPC, "an out buffer one byte short is refused");
  check (iec61937_mat_push (&mat, au, 9, out, sizeof out)
      == IEC61937_HBR_EINVAL, "a 9-byte AU is refused");
  check (iec61937_mat_push (&mat, au, IEC61937_MAT_MAX_AU + 2, out,
          sizeof out) == IEC61937_HBR_EINVAL,
      "an AU longer than its length field allows is refused");
  au[7] = 0xbc;
  check (iec61937_mat_push (&mat, au, first, out, sizeof out)
      == IEC61937_HBR_EINVAL, "a major sync of unknown type is refused");
  free (in);
}

int
main (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "usage: %s <dir with the iec61937-{dtshd,truehd,"
        "truehd96}.{in,ref} fixtures>\n", argv[0]);
    return 2;
  }
  test_dtshd (argv[1]);
  test_truehd (argv[1]);
  if (failures)
    return 1;
  printf ("ok - IEC 61937 HBR bursts match ffmpeg 9.0.2's spdif muxer\n");
  return 0;
}
