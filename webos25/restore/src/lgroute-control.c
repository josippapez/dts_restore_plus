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

/* 1 when the live watcher reports one of outputs (NULL-terminated); its
 * Digital Sound Output mode goes to mode. */
static int watched_output (const char *const outputs[], char mode[64])
{
  long pid;
  char output[64], proc[64];
  int i;
  FILE *file = fopen (LGROUTE_OUTPUT, "r");
  if (!file)
    return 0;
  int fields = fscanf (file, "%ld %63s %63s", &pid, output, mode);
  fclose (file);
  if (fields != 3 || pid <= 0)
    return 0;
  for (i = 0; outputs[i] && strcmp (output, outputs[i]); i++)
    ;
  if (!outputs[i])
    return 0;
  snprintf (proc, sizeof proc, "/proc/%ld", pid);
  return access (proc, F_OK) == 0;
}

/* 1 when the live watcher reports an external_arc output (ARC or eARC), the
 * only link wide enough for the HD carrier; its Digital Sound Output mode goes to mode. */
int lgroute_sound_output (char mode[64])
{
  static const char *const arc[] = { "external_arc", NULL };
  return watched_output (arc, mode);
}

/* 1 when the live watcher reports an output that carries the 2-channel
 * IEC 61937 DTS core: ARC/eARC, or optical (S/PDIF). Optical is owner-tested on
 * a 55UA73006LA (webOS 10.3.2, 33.31.80): the receiver showed DTS 5.1. */
int lgroute_core_output (char mode[64])
{
  static const char *const core[] = { "external_arc", "external_optical", NULL };
  return watched_output (core, mode);
}

int lgroute_control_receiver (void)
{
  char mode[64];
  /* In Auto, LG re-encodes and turns mixing back on with every policy rewrite. */
  return !access (LGROUTE_PT, F_OK) && !access (LGROUTE_HD, F_OK)
      && lgroute_sound_output (mode) && !strcmp (mode, "passThrough");
}

/* A tester's TV hung on a track switch after one -1, and the log could not
 * say which of these it was. */
static int ready (int fd, long output[5], char why[LGROUTE_WHY])
{
  long earc;
  if (lgroute_control_read (fd, "Sndout EARC OnOff", &earc, 1) < 0)
    snprintf (why, LGROUTE_WHY, "Sndout EARC OnOff could not be read: %s", strerror (errno));
  else if (earc != 1)
    snprintf (why, LGROUTE_WHY, "Sndout EARC OnOff is %ld, not on", earc);
  else if (lgroute_control_read (fd, "Sndout EARC Outputtype", output, 5) < 0)
    snprintf (why, LGROUTE_WHY, "Sndout EARC Outputtype could not be read: %s", strerror (errno));
  else if (!valid (output))
    snprintf (why, LGROUTE_WHY, "Sndout EARC Outputtype is %ld,%ld,%ld,%ld,%ld, not a known output type",
        output[0], output[1], output[2], output[3], output[4]);
  else
    return 1;
  return 0;
}

/* 1 = unmixed eARC, 0 = mixed, -1 = unavailable. */
int lgroute_control_unmixed_why (char why[LGROUTE_WHY])
{
  long output[5];
  int fd = open ("/dev/snd/controlC0", O_RDONLY | O_CLOEXEC);
  int ok = fd >= 0 && ready (fd, output, why);
  if (fd < 0)
    snprintf (why, LGROUTE_WHY, "/dev/snd/controlC0 could not be opened: %s", strerror (errno));
  else
    close (fd);
  return ok ? output[4] == 0 : -1;
}

/* Clears the mix word through LG's own setter; earc_route's probe keeps LG from
 * setting it again while the lease is held. 0 on success. */
int lgroute_control_unmix (void)
{
  struct snd_ctl_elem_value value;
  long output[5];
  int fd = open ("/dev/snd/controlC0", O_RDWR | O_CLOEXEC), ok = -1;
  memset (&value, 0, sizeof value);
  if (fd >= 0 && lgroute_control_read (fd, "Sndout EARC Outputtype", output, 5) == 0 && valid (output)
      && describe (fd, "Sndout EARC Outputtype", 5, &value.id) == 0) {
    for (int i = 0; i < 4; i++)
      value.value.integer.value[i] = output[i];
    ok = ioctl (fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0 ? -1 : 0;
  }
  if (fd >= 0)
    close (fd);
  return ok;
}

int lgroute_control_unmixed (void)
{
  char why[LGROUTE_WHY];
  return lgroute_control_unmixed_why (why);
}

#ifdef LGROUTE_CONTROL_MAIN
int main (int argc, char **argv)
{
  if (argc != 2 || strcmp (argv[1], "--check")) {
    fprintf (stderr, "usage: %s --check\n", argv[0]);
    return 2;
  }
  if (lgroute_control_unmixed () < 0) { perror ("lgroute-control"); return 1; }
  return 0;
}
#endif
