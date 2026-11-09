/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * edidaudio.c - keeps "Show DTS tracks in apps" off the TV's HDMI inputs.
 *
 * That opt-in sets tv.model.edidType to TrueHD+dts (or ac3+dts) so apps offer
 * DTS tracks. LG's eim builds every HDMI input's EDID from the same value, so
 * devices on those inputs are told the TV takes DTS, which it cannot play from
 * HDMI. Measured on a C5: a Google TV Streamer sent DTS 5.1 and the TV stayed
 * silent ("codec 48 (DTS) is not suppported").
 *
 * eim loads this library through LD_PRELOAD. libhdmi-conn-protocol calls
 * _EDID_CTRL_GetAudioType() through its PLT, so the definition below runs
 * instead and passes the same edidType without "+dts": the inputs get LG's
 * stock audio list, byte for byte. The numbers come from the library's own
 * {key, value, number} property table, read in memory, so a build whose
 * table differs is left alone.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>

#include "edidaudio.h"

/* A pointer is followed only into one of the ranges, and a string must end
 * inside the range it starts in. */
static const char *
range_str (const struct edidaudio_range *r, int n, uintptr_t p, size_t max)
{
  for (int i = 0; i < n; i++) {
    if (p < r[i].lo || p >= r[i].hi)
      continue;
    size_t room = r[i].hi - p;
    if (room > max)
      room = max;
    return memchr ((const void *) p, '\0', room) ? (const char *) p : NULL;
  }
  return NULL;
}

int
edidaudio_scan (const struct edidaudio_range *r, int n, struct edidaudio_map *m)
{
  const size_t w = sizeof (void *);
  int truehd_dts = -1, truehd = -1, ac3_dts = -1, ac3 = -1;

  for (int i = 0; i < n; i++) {
    for (uintptr_t a = (r[i].lo + w - 1) & ~(uintptr_t) (w - 1); a + 3 * w <= r[i].hi; a += w) {
      uintptr_t key, value;
      int32_t number;
      memcpy (&key, (const void *) a, w);
      const char *k = range_str (r, n, key, 32);
      if (!k || strcmp (k, "audio_edid_type") != 0)
        continue;
      memcpy (&value, (const void *) (a + w), w);
      memcpy (&number, (const void *) (a + 2 * w), sizeof number);
      const char *v = range_str (r, n, value, 32);
      if (!v)
        continue;
      if (strcmp (v, "TrueHD+dts") == 0)
        truehd_dts = number;
      else if (strcmp (v, "TrueHD") == 0)
        truehd = number;
      else if (strcmp (v, "ac3+dts") == 0)
        ac3_dts = number;
      else if (strcmp (v, "ac3") == 0)
        ac3 = number;
    }
  }
  if (truehd_dts < 0 || truehd < 0 || ac3_dts < 0 || ac3 < 0 ||
      truehd_dts == truehd || ac3_dts == ac3)
    return 0;
  m->truehd_dts = truehd_dts;
  m->truehd = truehd;
  m->ac3_dts = ac3_dts;
  m->ac3 = ac3;
  return 1;
}

#ifndef EDIDAUDIO_NO_HOOK
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#define SYM "_Z23_EDID_CTRL_GetAudioType23EDID_AUDIO_INPUT_TYPE_Tb23EDID_AUDIO_CAPABILITY_Tbbbb"

typedef int (*get_audio_type_fn) (int, bool, int, bool, bool, bool, bool);

static get_audio_type_fn real;
static struct edidaudio_map map;
static int remap;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static int
find_lib (struct dl_phdr_info *info, size_t size, void *data)
{
  (void) size;
  if (!info->dlpi_name || !strstr (info->dlpi_name, "libhdmi-conn-protocol.so"))
    return 0;
  struct edidaudio_range r[8];
  int n = 0;
  for (int i = 0; i < info->dlpi_phnum && n < 8; i++) {
    const ElfW (Phdr) *p = &info->dlpi_phdr[i];
    if (p->p_type == PT_LOAD && (p->p_flags & PF_R)) {
      r[n].lo = info->dlpi_addr + p->p_vaddr;
      r[n].hi = r[n].lo + p->p_memsz;
      n++;
    }
  }
  *(int *) data = edidaudio_scan (r, n, &map);
  return 1;
}

static void
init (void)
{
  real = (get_audio_type_fn) dlsym (RTLD_NEXT, SYM);
  dl_iterate_phdr (find_lib, &remap);
  /* For the boot script and report-tv.sh. */
  mkdir (EDIDAUDIO_DIR, 0755);
  int fd = open (EDIDAUDIO_STATE, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  FILE *f = fd >= 0 ? fdopen (fd, "w") : NULL;
  if (f) {
    if (remap)
      fprintf (f, "remap TrueHD+dts=%d->%d ac3+dts=%d->%d\n", map.truehd_dts, map.truehd,
          map.ac3_dts, map.ac3);
    else
      fputs ("off: libhdmi-conn-protocol's edidType table not recognised\n", f);
    fclose (f);
  }
}

int
_Z23_EDID_CTRL_GetAudioType23EDID_AUDIO_INPUT_TYPE_Tb23EDID_AUDIO_CAPABILITY_Tbbbb (int input,
    bool a, int type, bool b, bool c, bool d, bool e)
{
  pthread_once (&once, init);
  if (remap) {
    if (type == map.truehd_dts)
      type = map.truehd;
    else if (type == map.ac3_dts)
      type = map.ac3;
  }
  return real (input, a, type, b, c, d, e);
}
#endif
