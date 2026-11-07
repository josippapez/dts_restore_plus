/* Named ALSA controls only; LG owns every DSP connection.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute-control.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LGROUTE_LUNA_SEND
#define LGROUTE_LUNA_SEND "/usr/bin/luna-send"
#endif

extern char **environ;

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

/* One luna-send call; the reply is read to EOF so a SIGCHLD-ignoring host still works. */
static int luna (const char *uri, const char *payload, char *reply, size_t size)
{
  char *argv[] = { "luna-send", "-n", "1", "-w", "2000", (char *) uri, (char *) payload, NULL };
  char *env[64];
  size_t count = 0, used = 0;
  int out[2];
  pid_t pid;
  posix_spawn_file_actions_t actions;
  for (char **e = environ; *e && count < 63; e++)
    /* LG's LD_PRELOAD in the media pipeline breaks luna-send. */
    if (strncmp (*e, "LD_PRELOAD=", 11))
      env[count++] = *e;
  env[count] = NULL;
  if (pipe2 (out, O_CLOEXEC) < 0)
    return -1;
  posix_spawn_file_actions_init (&actions);
  posix_spawn_file_actions_addopen (&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2 (&actions, out[1], 1);
  int spawned = posix_spawn (&pid, LGROUTE_LUNA_SEND, &actions, NULL, argv, env);
  posix_spawn_file_actions_destroy (&actions);
  close (out[1]);
  if (spawned) { close (out[0]); return -1; }
  for (ssize_t n; used + 1 < size && (n = read (out[0], reply + used, size - used - 1)) != 0;) {
    if (n < 0) { if (errno == EINTR) continue; break; }
    used += n;
  }
  reply[used] = 0;
  close (out[0]);
  while (waitpid (pid, NULL, 0) < 0 && errno == EINTR) {}
  return strstr (reply, "\"returnValue\":true") ? 0 : -1;
}

static int copy_field (const char *object, const char *end, const char *key, char *value, size_t size)
{
  char pattern[48];
  snprintf (pattern, sizeof pattern, "\"%s\":\"", key);
  const char *start = strstr (object, pattern);
  if (!start || start > end)
    return -1;
  start += strlen (pattern);
  const char *stop = strchr (start, '"');
  if (!stop || stop > end || (size_t) (stop - start) >= size)
    return -1;
  memcpy (value, start, stop - start);
  value[stop - start] = 0;
  return 0;
}

/* starfish-media-pipeline is started with -scom.webos.pipeline.<pipelineId>. */
int lgroute_input_pipeline (char *pipeline, size_t size)
{
  static const char prefix[] = "-scom.webos.pipeline.";
  char args[4096];
  int fd = open ("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  ssize_t length = fd >= 0 ? read (fd, args, sizeof args - 1) : -1;
  if (fd >= 0) close (fd);
  if (length <= 0)
    return -1;
  args[length] = 0;
  for (char *arg = args; arg < args + length; arg += strlen (arg) + 1)
    if (!strncmp (arg, prefix, sizeof prefix - 1) && strlen (arg + sizeof prefix - 1) < size
        && arg[sizeof prefix - 1]) {
      strcpy (pipeline, arg + sizeof prefix - 1);
      return 0;
    }
  return -1;
}

/* LG's own report for this pipeline, restored when HD transport ends. */
int lgroute_input_query (const char *pipeline, char *codec, size_t codec_size,
    char *immersive, size_t immersive_size)
{
  char reply[4096], key[96];
  if (luna ("luna://com.webos.service.audio/tv/getAudioInputProperty", "{}", reply, sizeof reply) < 0)
    return -1;
  snprintf (key, sizeof key, "\"pipelineId\":\"%s\"", pipeline);
  char *match = strstr (reply, key);
  if (!match)
    return -1;
  char *object = match, *end = strchr (match, '}');
  while (object > reply && *object != '{')
    object--;
  if (!end || copy_field (object, end, "codec", codec, codec_size) < 0)
    return -1;
  if (copy_field (object, end, "immersive", immersive, immersive_size) < 0)
    snprintf (immersive, immersive_size, "none");
  return 0;
}

static int token (const char *value)
{
  if (!value || !*value)
    return 0;
  for (; *value; value++)
    if (!(*value >= 'a' && *value <= 'z') && !(*value >= 'A' && *value <= 'Z')
        && !(*value >= '0' && *value <= '9') && !strchr ("_.-", *value))
      return 0;
  return 1;
}

/* audiod forwards this to UMI, which reapplies the digital output policy. */
int lgroute_input_set (const char *pipeline, const char *resource,
    const char *codec, const char *channel, const char *immersive)
{
  char payload[512], reply[256];
  if (!token (pipeline) || !token (resource) || !token (codec) || !token (immersive)
      || (channel && !token (channel)))
    return -1;
  int length = snprintf (payload, sizeof payload,
      "{\"categoryType\":\"audioInputInfo\",\"requestType\":\"setAudioInputInfo\","
      "\"pipelineInfo\":{\"pipelineId\":\"%s\",\"resource\":\"%s\",\"sourceType\":\"MEDIA\"},"
      "\"categoryInfo\":{\"codec\":\"%s\"%s%s%s,\"immersive\":\"%s\"}}",
      pipeline, resource, codec, channel ? ",\"channel\":\"" : "",
      channel ? channel : "", channel ? "\"" : "", immersive);
  if (length < 0 || (size_t) length >= sizeof payload)
    return -1;
  return luna ("luna://com.webos.service.audio/tv/setQualityAttribute", payload, reply, sizeof reply);
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
