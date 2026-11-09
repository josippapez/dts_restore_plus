/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host test for edidaudio_scan(): finds libhdmi-conn-protocol's edidType
 * numbers from its {key, value, number} table, and refuses anything else. */
#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EDIDAUDIO_NO_HOOK
#include "../edidaudio.c"

struct entry { const char *key, *value; int32_t number; };

/* One buffer standing in for the library's mapped segments: strings first,
 * then the table, so every pointer in the table lands inside the range. */
static alignas (16) char seg[4096];

static const char *
put (size_t *off, const char *s)
{
  char *p = seg + *off;
  strcpy (p, s);
  *off += strlen (s) + 1;
  return p;
}

/* key "@" stands for another table's key. */
static int
scan (const struct entry *e, int n, struct edidaudio_map *m)
{
  memset (seg, 0, sizeof seg);
  size_t off = 0;
  const char *key = put (&off, "audio_edid_type");
  const char *other = put (&off, "sound_mode");
  size_t toff = (off + 15) & ~(size_t) 15;
  struct entry *t = (struct entry *) (seg + toff);
  size_t voff = toff + (size_t) n * sizeof *t;
  for (int i = 0; i < n; i++) {
    t[i].key = e[i].key[0] == '@' ? other : key;
    t[i].value = put (&voff, e[i].value);
    t[i].number = e[i].number;
  }
  struct edidaudio_range r = { (uintptr_t) seg, (uintptr_t) seg + sizeof seg };
  return edidaudio_scan (&r, 1, m);
}

int
main (void)
{
  struct edidaudio_map m;

  /* The C5 33.31.68 table, in the library's own order. */
  const struct entry c5[] = {
    { "k", "pcm", 0 }, { "k", "ac3", 1 }, { "k", "ac3+dts", 2 }, { "k", "TrueHD", 3 },
    { "k", "TrueHD+dts", 4 }, { "k", "default", 0 }, { "@", "LG Sound Engine", 0 },
  };
  assert (scan (c5, 7, &m) == 1);
  assert (m.truehd_dts == 4 && m.truehd == 3 && m.ac3_dts == 2 && m.ac3 == 1);

  /* Other numbers are taken from the table, not assumed. */
  const struct entry renumbered[] = {
    { "k", "ac3", 6 }, { "k", "ac3+dts", 9 }, { "k", "TrueHD", 5 }, { "k", "TrueHD+dts", 7 },
  };
  assert (scan (renumbered, 4, &m) == 1);
  assert (m.truehd_dts == 7 && m.truehd == 5 && m.ac3_dts == 9 && m.ac3 == 6);

  /* A missing form leaves the library alone. */
  const struct entry missing[] = {
    { "k", "ac3", 1 }, { "k", "ac3+dts", 2 }, { "k", "TrueHD", 3 },
  };
  assert (scan (missing, 3, &m) == 0);

  /* The same values under another key do not count. */
  const struct entry wrong_key[] = {
    { "@", "ac3", 1 }, { "@", "ac3+dts", 2 }, { "@", "TrueHD", 3 }, { "@", "TrueHD+dts", 4 },
  };
  assert (scan (wrong_key, 4, &m) == 0);

  /* "+dts" mapped to the plain number would change nothing: refuse it. */
  const struct entry same[] = {
    { "k", "ac3", 1 }, { "k", "ac3+dts", 1 }, { "k", "TrueHD", 3 }, { "k", "TrueHD+dts", 3 },
  };
  assert (scan (same, 4, &m) == 0);

  /* A pointer outside the ranges is not followed. */
  static const char outside[] = "audio_edid_type";
  struct entry stray[4] = {
    { outside, "ac3", 1 }, { outside, "ac3+dts", 2 }, { outside, "TrueHD", 3 }, { outside, "TrueHD+dts", 4 },
  };
  memset (seg, 0, sizeof seg);
  memcpy (seg + 64, stray, sizeof stray);
  struct edidaudio_range r = { (uintptr_t) seg, (uintptr_t) seg + sizeof seg };
  assert (edidaudio_scan (&r, 1, &m) == 0);

  puts ("test-edidaudio: ok");
  return 0;
}
