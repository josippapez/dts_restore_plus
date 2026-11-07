/* Named ALSA controls only; LG owns every DSP connection.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute-control.h"
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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

static int lgroute_control_write (int fd, const char *name, const long *values, unsigned count)
{
  struct snd_ctl_elem_value value;
  unsigned i;
  memset (&value, 0, sizeof value);
  if (count > 128 || describe (fd, name, count, &value.id) < 0)
    return -1;
  for (i = 0; i < count; i++)
    value.value.integer.value[i] = values[i];
  return ioctl (fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value);
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
      || (strcmp (mode, "passThrough") && strcmp (mode, "auto")))
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

int lgroute_control_check (void)
{
  int fd = open ("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;
  long output[5];
  int ok = ready (fd, output);
  close (fd);
  return ok ? 0 : -1;
}

static int journal_read (long before[5], long applied[5])
{
  int fd = open (LGROUTE_STATE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return errno == ENOENT ? 0 : -1;
  FILE *file = fdopen (fd, "r");
  if (!file) { close (fd); return -1; }
  int n = fscanf (file, "LGR1 %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
      before, before + 1, before + 2, before + 3, before + 4,
      applied, applied + 1, applied + 2, applied + 3, applied + 4);
  int trailing;
  do { trailing = fgetc (file); } while (trailing == ' ' || trailing == '\n' || trailing == '\t' || trailing == '\r');
  fclose (file);
  if (n != 10 || trailing != EOF || !valid (before) || !valid (applied)
      || memcmp (before, applied, 4 * sizeof *before) || applied[4] != 0) {
    errno = EINVAL;
    return -1;
  }
  return 1;
}

static int restore (int fd)
{
  long before[5], applied[5], current[5];
  int exists = journal_read (before, applied);
  if (exists <= 0)
    return exists;
  if (lgroute_control_read (fd, "Sndout EARC Outputtype", current, 5) < 0)
    return -1;
  /* Do not overwrite a user's changed codec/channel/output policy. */
  if (!memcmp (current, applied, sizeof current)) {
    current[4] = before[4];
    if (lgroute_control_write (fd, "Sndout EARC Outputtype", current, 5) < 0
        || lgroute_control_read (fd, "Sndout EARC Outputtype", current, 5) < 0
        || memcmp (current, before, sizeof current))
      return -1;
  }
  return unlink (LGROUTE_STATE);
}

int lgroute_control_open (LgRouteControl *control)
{
  control->fd = control->lock_fd = -1;
  if (mkdir (LGROUTE_RUN, 0755) < 0 && errno != EEXIST)
    return -1;
  control->lock_fd = open (LGROUTE_LOCK, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (control->lock_fd < 0 || flock (control->lock_fd, LOCK_EX | LOCK_NB) < 0)
    goto fail;
  control->fd = open ("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
  long output[5];
  if (control->fd < 0 || restore (control->fd) < 0 || !ready (control->fd, output))
    goto fail;
  return 0;
fail:
  if (control->fd >= 0) close (control->fd);
  if (control->lock_fd >= 0) close (control->lock_fd);
  control->fd = control->lock_fd = -1;
  return -1;
}

int lgroute_control_apply (LgRouteControl *control)
{
  long current[5], before[5], applied[5];
  if (control->fd < 0 || !lgroute_control_receiver () || !ready (control->fd, current))
    return -1;
  int existing = journal_read (before, applied);
  if (existing < 0)
    return -1;
  if (existing > 0) {
    if (memcmp (current, applied, 4 * sizeof *current)) {
      errno = EBUSY;
      return -1;
    }
    /* LG re-enables mixing while the stream starts; reassert only our own bit. */
    if (current[4] == applied[4])
      return 0;
    return lgroute_control_write (control->fd, "Sndout EARC Outputtype", applied, 5) == 0
        && lgroute_control_read (control->fd, "Sndout EARC Outputtype", current, 5) == 0
        && !memcmp (current, applied, sizeof current) ? 0 : -1;
  }
  memcpy (before, current, sizeof current);
  memcpy (applied, current, sizeof current);
  applied[4] = 0;
  char temporary[128];
  snprintf (temporary, sizeof temporary, LGROUTE_STATE ".%ld", (long) getpid ());
  int fd = open (temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -1;
  FILE *file = fdopen (fd, "w");
  if (!file) { close (fd); unlink (temporary); return -1; }
  int ok = fprintf (file, "LGR1 %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n",
      before[0], before[1], before[2], before[3], before[4],
      applied[0], applied[1], applied[2], applied[3], applied[4]) > 0;
  if (fflush (file) || fsync (fd)) ok = 0;
  if (fclose (file)) ok = 0;
  if (!ok || rename (temporary, LGROUTE_STATE) < 0) { unlink (temporary); return -1; }
  if ((current[4] != 0 && lgroute_control_write (control->fd, "Sndout EARC Outputtype", applied, 5) < 0)
      || lgroute_control_read (control->fd, "Sndout EARC Outputtype", current, 5) < 0
      || memcmp (current, applied, sizeof current))
    return -1;
  return 0;
}

int lgroute_control_release (LgRouteControl *control)
{
  int result = control->fd >= 0 ? restore (control->fd) : 0;
  if (control->fd >= 0) close (control->fd);
  if (control->lock_fd >= 0) close (control->lock_fd);
  control->fd = control->lock_fd = -1;
  return result;
}

int lgroute_control_recover (void)
{
  if (access (LGROUTE_STATE, F_OK))
    return errno == ENOENT ? 0 : -1;
  LgRouteControl control = {-1, -1};
  control.lock_fd = open (LGROUTE_LOCK, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (control.lock_fd < 0)
    return -1;
  if (flock (control.lock_fd, LOCK_EX | LOCK_NB) < 0) {
    int busy = errno == EWOULDBLOCK || errno == EAGAIN;
    close (control.lock_fd);
    return busy ? 0 : -1;
  }
  control.fd = open ("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
  if (control.fd < 0) { close (control.lock_fd); return -1; }
  return lgroute_control_release (&control);
}

#ifdef LGROUTE_CONTROL_MAIN
int main (int argc, char **argv)
{
  int result;
  if (argc != 2 || (strcmp (argv[1], "--recover") && strcmp (argv[1], "--check"))) {
    fprintf (stderr, "usage: %s --recover|--check\n", argv[0]);
    return 2;
  }
  result = !strcmp (argv[1], "--recover") ? lgroute_control_recover () : lgroute_control_check ();
  if (result < 0) { perror ("lgroute-control"); return 1; }
  return 0;
}
#endif
