/* Named ALSA controls only; LG owns every DSP connection.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute-control.h"
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int describe (int fd, const char *name, unsigned count,
    struct snd_ctl_elem_id *id)
{
  struct snd_ctl_elem_info info;
  memset (&info, 0, sizeof info);
  info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
  snprintf ((char *) info.id.name, sizeof info.id.name, "%s", name);
  if (ioctl (fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
    return -1;
  if (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER || info.count != count
      || (info.access & SNDRV_CTL_ELEM_ACCESS_READWRITE) != SNDRV_CTL_ELEM_ACCESS_READWRITE) {
    errno = ENOTSUP;
    return -1;
  }
  *id = info.id;
  return 0;
}

static int lgroute_control_read (int fd, const char *name, long *values, unsigned count)
{
  struct snd_ctl_elem_value value;
  unsigned i;
  memset (&value, 0, sizeof value);
  if (count > 128 || describe (fd, name, count, &value.id) < 0
      || ioctl (fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) < 0)
    return -1;
  for (i = 0; i < count; i++)
    values[i] = value.value.integer.value[i];
  return 0;
}

static int valid (const long output[5])
{
  return output[0] >= 0 && output[0] <= 2 && output[1] >= 1 && output[1] <= 9
      && output[2] >= 0 && output[2] <= 8 && output[3] >= 0 && output[3] <= 192000
      && (output[4] == 0 || output[4] == 1);
}

int lgroute_control_receiver (void)
{
  FILE *file;
  long pid;
  char output[64], mode[64], proc[64];
  if (access (LGROUTE_PT, F_OK) || access (LGROUTE_HD, F_OK))
    return 0;
  file = fopen (LGROUTE_OUTPUT, "r");
  if (!file)
    return 0;
  int fields = fscanf (file, "%ld %63s %63s", &pid, output, mode);
  fclose (file);
  if (fields != 3 || pid <= 0 || strcmp (output, "external_arc")
      /* In Auto, LG re-encodes and turns mixing back on with every policy rewrite. */
      || strcmp (mode, "passThrough"))
    return 0;
  snprintf (proc, sizeof proc, "/proc/%ld", pid);
  return access (proc, F_OK) == 0;
}

static int ready (int fd, long output[5])
{
  long earc;
  return lgroute_control_read (fd, "Sndout EARC OnOff", &earc, 1) == 0 && earc == 1
      && lgroute_control_read (fd, "Sndout EARC Outputtype", output, 5) == 0
      && valid (output);
}

/* 1 = LG requests unmixed eARC, 0 = mixed, -1 = unavailable. Never written:
 * in Pass Through LG derives the mixer word from its own setMixer state. */
int lgroute_control_unmixed (void)
{
  long output[5];
  int fd = open ("/dev/snd/controlC0", O_RDONLY | O_CLOEXEC);
  int ok = fd >= 0 && ready (fd, output);
  if (fd >= 0) close (fd);
  return ok ? output[4] == 0 : -1;
}

int lgroute_control_check (void)
{
  return lgroute_control_unmixed () < 0 ? -1 : 0;
}

#ifdef LGROUTE_CONTROL_MAIN
int main (int argc, char **argv)
{
  int result;
  if (argc != 2 || strcmp (argv[1], "--check")) {
    fprintf (stderr, "usage: %s --check\n", argv[0]);
    return 2;
  }
  result = lgroute_control_check ();
  if (result < 0) { perror ("lgroute-control"); return 1; }
  return 0;
}
#endif
