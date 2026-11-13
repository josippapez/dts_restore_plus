/* LG-managed PCM bypass transport. The first ES event survives the PCM-only
 * RTS/converter; lipsync retypes subsequent carrier data as ES.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute.h"
#include "lgroute-graph.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MODULE_GRAPH "/proc/aaudio/dot_module_connection"
#ifndef LGROUTE_INSTANCES
#define LGROUTE_INSTANCES "/proc/aaudio/instances"
#endif
#define SINK_PLUGIN "/usr/lib/gstreamer-1.0/libgstlgaudiosink.so"
#ifndef LGROUTE_SINK_WAIT
#define LGROUTE_SINK_WAIT (2 * G_TIME_SPAN_SECOND)
#endif
#ifndef LGROUTE_AREN
#define LGROUTE_AREN "/dev/lg/aren0"
#endif
/* _IOR('R', 0x23, u32): the last module of this renderer's bypass chain, its
 * lipsync. A passive read (aren_get_tail_uid, renderer_get_uid_bypass_tail);
 * the same number in the 33.31.x kernels of the C5, G5 and G2/C2. */
#define AREN_GET_BYPASS_TAIL_UID 0x80045223u
#define ES_FLAG (1u << 21)
/* Flushes are not GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM; a seek or resume sends them. */
#define EVENT_PROBE_MASK (GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH)

GST_DEBUG_CATEGORY_STATIC (lgroute_debug);
#define GST_CAT_DEFAULT (lgroute_debug)
/* LG's debug config never routes our category to legacy-log, so testers only see this file. */
#define ROUTE_LOG LGROUTE_RUN "/lgroute.log"
/* The carrier's bytes per second: 192 kHz, 8 channels, 16 bit. */
#define CARRIER_BYTES_PER_SECOND (192000 * 8 * 2)
#define PACE_WINDOW (GST_SECOND / 4)
/* How long after LG's own track-change restart ours may follow. */
#define RESTART_SETTLE (500 * G_TIME_SPAN_MILLISECOND)
#ifndef LGROUTE_SNDOUT
#define LGROUTE_SNDOUT "/proc/lgtv-driver/audio/sndout"
#endif
/* This TV's kernel has reported its bypass route in LGROUTE_SNDOUT since boot. */
#define BYPASS_SEEN LGROUTE_RUN "/bypass-seen"
#ifndef LGROUTE_APP_ROUTE
#define LGROUTE_APP_ROUTE "/sys/module/earc_route/parameters/app_route"
#endif

/* earc_route forces LG's eARC bypass for the leased renderer, so LG never mixes
 * the carrier as a codec the receiver lacks. TRUE when the module took the value. */
static gboolean app_lease (gint value)
{
  gchar text[4];
  int fd = open (LGROUTE_APP_ROUTE, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return FALSE;
  gint length = g_snprintf (text, sizeof text, "%d\n", value);
  gboolean ok = write (fd, text, length) == length;
  if (!ok)
    GST_DEBUG ("lgroute: app_route write failed");
  close (fd);
  return ok;
}

static void vnote (const gchar *format, va_list args)
{
  FILE *log = fopen (ROUTE_LOG, "a");
  if (log && fseek (log, 0, SEEK_END) == 0 && ftell (log) > 65536)
    log = freopen (ROUTE_LOG, "w", log);
  if (!log)
    return;
  GDateTime *now = g_date_time_new_now_local ();
  gchar *when = g_date_time_format (now, "%T.%f");
  g_date_time_unref (now);
  fprintf (log, "%.12s ", when);
  vfprintf (log, format, args);
  fputc ('\n', log);
  g_free (when);
  fclose (log);
}

static void note (const gchar *format, ...) G_GNUC_PRINTF (1, 2);
static void note (const gchar *format, ...)
{
  va_list args;
  va_start (args, format);
  vnote (format, args);
  va_end (args);
}

void lgroute_log (const gchar *format, ...)
{
  va_list args;
  va_start (args, format);
  vnote (format, args);
  va_end (args);
}

/* Guards the LgRoute fields that name what is armed (sink, sinkpad, probe ids,
 * resource). Never held across a GStreamer call. */
static GMutex route_lock;

/* The boot script hashes SINK_PLUGIN once and writes LGROUTE_SINK_OK on a match. */
static gboolean verified_sink (GstElement *sink)
{
  GstElementFactory *factory = gst_element_get_factory (sink);
  GstPlugin *plugin;
  gboolean ok;
  if (!factory || strcmp (GST_OBJECT_NAME (factory), "audiosink")
      || !g_object_class_find_property (G_OBJECT_GET_CLASS (sink), "pcm-multi-ch"))
    return FALSE;
  plugin = gst_plugin_feature_get_plugin (GST_PLUGIN_FEATURE (factory));
  if (!plugin)
    return FALSE;
  ok = !g_strcmp0 (gst_plugin_get_filename (plugin), SINK_PLUGIN)
      && access (LGROUTE_SINK_OK, F_OK) == 0;
  gst_object_unref (plugin);
  return ok;
}

GstCaps *lgroute_carrier_caps (void)
{
  return gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
      "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 192000,
      "channels", G_TYPE_INT, 8, "channel-mask", GST_TYPE_BITMASK, (guint64) 0xc3f,
      "acmod", G_TYPE_UINT, (guint) 0, "es", G_TYPE_BOOLEAN, TRUE,
      "codec-str", G_TYPE_STRING, "mat", "clock-x4", G_TYPE_BOOLEAN, TRUE,
      "data-rate", G_TYPE_INT, 192000, "data-channels", G_TYPE_INT, 8,
      "data-bps", G_TYPE_INT, 16, NULL);
}

/* LG builds a new converter whenever it rebuilds the renderer. The rebuilt one
 * lacks our setup, and LG's next start event describes plain PCM because the
 * carrier needs pcm-multi-ch on (lgao_push_first_event), so the receiver gets
 * noise. One such rebuild was seen mid-play after a resume. */
static gboolean converter_present (LgRoute *route)
{
  gchar *path = g_strdup_printf (LGROUTE_INSTANCES "/%s", route->converter);
  gboolean present = route->converter[0] && g_file_test (path, G_FILE_TEST_IS_DIR);
  g_free (path);
  return present;
}

/* LG's sink keeps the one /dev/lg/aren0 descriptor of this process open for as
 * long as its renderer exists; nothing else here opens that device. */
static gboolean own_bypass_tail (gchar uid[9])
{
  DIR *dir = opendir ("/proc/self/fd");
  int fd = -1, count = 0;
  for (struct dirent *entry; dir && (entry = readdir (dir));) {
    gchar path[64], target[64];
    g_snprintf (path, sizeof path, "/proc/self/fd/%s", entry->d_name);
    ssize_t n = readlink (path, target, sizeof target - 1);
    if (n > 0 && (target[n] = 0, !strcmp (target, LGROUTE_AREN)) && count++ == 0)
      fd = atoi (entry->d_name);
  }
  if (dir) closedir (dir);
  guint32 tail = 0;
  if (count != 1 || ioctl (fd, AREN_GET_BYPASS_TAIL_UID, &tail) < 0 || !tail || tail == G_MAXUINT32)
    return FALSE;
  g_snprintf (uid, 9, "%08x", tail);
  return TRUE;
}

/* PCMCVT_CMD_SET_ALLFMT: 192 kHz, 8 ch, 16-bit in and out; LG's gain is left alone. */
static gboolean configure_converter (LgRoute *route)
{
  static const char cmd[] = "0x50020020 192000 8 16 0 0 192000 8 16\n";
  gchar *graph = NULL, *uid = route->converter, tail[9];
  uid[0] = 0;
  if (!g_file_get_contents (MODULE_GRAPH, &graph, NULL, NULL))
    return FALSE;
  /* Another loaded pipeline can hold a PCM bypass chain of its own (the app's
   * sample player left one, and a movie in another app then found two). The
   * kernel names ours by its lipsync. Once it has, and the graph shows that
   * lipsync, a chain ending anywhere else belongs to someone else: without a
   * converter on ours, LG did not build our PCM bypass, and the only chain in
   * the graph must not be taken for it. Where the kernel cannot say, the one
   * chain in the graph is ours, as before. */
  gchar *node = own_bypass_tail (tail) ? g_strdup_printf ("lipsync_%s_", tail) : NULL;
  gboolean own = node && strstr (graph, node);
  gboolean ok = lgroute_bypass_converter (graph, own ? tail : NULL, uid);
  if (own && !ok)
    note ("this renderer's bypass (lipsync %s) has no PCM converter", tail);
  route->own = own;
  g_free (node);
  g_free (graph);
  if (!ok)
    return FALSE;
  gchar *path = g_strdup_printf (LGROUTE_INSTANCES "/%s/cmd", uid);
  int fd = open (path, O_WRONLY | O_CLOEXEC);
  g_free (path);
  ok = fd >= 0 && write (fd, cmd, sizeof cmd - 1) == (ssize_t) (sizeof cmd - 1);
  if (fd >= 0) close (fd);
  return ok;
}

/* The sink is read under the lock but driven outside it, so a disarm on another
 * thread can neither free it under us nor wait on a pad we are blocked in. */
static void set_pcm (LgRoute *route, gboolean on)
{
  /* A failed route only waits for its disarm. A tester's TV logged a failed
   * route turning LG's bypass on for the next track as its audio locked up. */
  if (route->failed)
    return;
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink ? gst_object_ref (route->sink) : NULL;
  g_mutex_unlock (&route_lock);
  if (!sink)
    return;
  g_object_set (sink, "pcm-multi-ch", on, NULL);
  gst_object_unref (sink);
}

static GstPadProbeReturn silence (GstPadProbeInfo *info)
{
  GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
  if (!buffer)
    return GST_PAD_PROBE_DROP;
  GST_PAD_PROBE_INFO_DATA (info) = buffer;
  gst_buffer_memset (buffer, 0, 0, gst_buffer_get_size (buffer));
  return GST_PAD_PROBE_OK;
}

/* The [ARC] section of LG's sound-output status, or NULL. */
static gchar *arc_status (void)
{
  gchar *file = NULL, *section, *end;
  if (!g_file_get_contents (LGROUTE_SNDOUT, &file, NULL, NULL))
    return NULL;
  section = strstr (file, "[ARC]\n");
  if (section && (end = strstr (section, "\n[")))
    *end = 0;
  section = g_strdup (section);
  g_free (file);
  return section;
}

static gboolean arc_value (const gchar *status, const gchar *name, gchar value[32])
{
  gchar *key = g_strdup_printf ("\n%s=", name);
  const gchar *found = status ? strstr (status, key) : NULL;
  if (found) {
    found += strlen (key);
    g_strlcpy (value, found, MIN (strcspn (found, "\n") + 1, 32));
  }
  g_free (key);
  return found != NULL;
}

/* LG's own account of its ARC/eARC output, for the route log: receiver testers
 * mostly send this log, and it is where the kernel says what it did. */
static gchar *lg_output (void)
{
  static const gchar *const names[] = { "ArcMode", "OutputCodec", "OutputChannel", "Mute", "Underflow" };
  GString *text = g_string_new (NULL);
  gchar *status = arc_status (), value[32];
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (arc_value (status, names[i], value))
      g_string_append_printf (text, " %s=%s", names[i], value);
  g_free (status);
  if (text->len)
    g_string_prepend (text, "; LG output:");
  return g_string_free (text, FALSE);
}

/* The route LG's kernel took to the ARC/eARC output: 1 = its bypass (renderer
 * bypass port -> mixer bypass port -> output, shown as "Pass-Through"), 0 = its
 * mixer or an encoder (shown as that output codec), -1 = not shown. */
static int lg_bypassing (void)
{
  static const gchar *const mixed[] = { "PCM", "DD", "DDP", "AAC", "MAT" };
  gchar *status = arc_status (), codec[32];
  int state = -1;
  if (arc_value (status, "OutputCodec", codec)) {
    if (!strcmp (codec, "Pass-Through"))
      state = 1;
    for (guint i = 0; i < G_N_ELEMENTS (mixed); i++)
      if (!strcmp (codec, mixed[i]))
        state = 0;
  }
  g_free (status);
  return state;
}

/* Every give-up goes through here so a tester's log names the reason. */
static void route_fail (LgRoute *route, const gchar *reason)
{
  route->failed = TRUE;
  route->reason = reason;
  GST_WARNING_OBJECT (route->element, "lgroute: HD transport failed: %s", reason);
  note ("failed, decoding instead: %s", reason);
}

static gboolean pad_has_carrier (GstPad *pad)
{
  GstCaps *current = pad ? gst_pad_get_current_caps (pad) : NULL, *carrier = lgroute_carrier_caps ();
  gboolean same = current && gst_caps_is_equal (current, carrier);
  gst_caps_unref (carrier);
  if (current) gst_caps_unref (current);
  return same;
}

static void restart_pace (LgRoute *route)
{
  route->window_start = route->last_push = 0;
  route->window_media = 0;
  route->slow_windows = 0;
}

static gboolean sink_playing (LgRoute *route)
{
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink ? gst_object_ref (route->sink) : NULL;
  g_mutex_unlock (&route_lock);
  gboolean playing = sink && GST_STATE (sink) == GST_STATE_PLAYING
      && GST_STATE_PENDING (sink) == GST_STATE_VOID_PENDING;
  if (sink) gst_object_unref (sink);
  return playing;
}

/* After a resume or a rebuild, have LG's sink restart its renderer the way it
 * does on a track switch, so the next buffer sends a fresh bitstream start.
 * Twice at most; after that the decoder takes over. */
static gboolean restart_wanted (LgRoute *route, const gchar *why)
{
  if (route->restarts >= 2)
    return FALSE;
  route->restart_pending = TRUE;
  route->restart_after = 0;
  note ("%s: restarting LG's renderer once the sink plays", why);
  return TRUE;
}

/* A new stream id is what makes LG's sink close, reopen and restart the
 * renderer; event_probe sees it too and redoes our setup on the next buffer. */
static gboolean restart_renderer (LgRoute *route)
{
  g_mutex_lock (&route_lock);
  GstPad *pad = route->sinkpad ? gst_object_ref (route->sinkpad) : NULL;
  g_mutex_unlock (&route_lock);
  if (!pad)
    return FALSE;
  route->restart_pending = FALSE;
  gchar *id = g_strdup_printf ("%.100s/lgroute-restart%u", route->stream_id, ++route->restarts);
  gboolean sent = gst_pad_send_event (pad, gst_event_new_stream_start (id));
  g_free (id);
  gst_object_unref (pad);
  return sent && route->first;
}

/* LG plays the carrier at its own rate, so the pushes keep that pace. After a
 * resume one TV took four times as long, fell 10 s behind and the receiver
 * played noise: three slow windows in a row mean that, not a stall or a pause,
 * which show up as a single long gap. */
static gboolean carrier_slow (LgRoute *route, gsize size, gint64 now)
{
  gboolean playing = sink_playing (route);
  gint64 gap = now - route->last_push;
  route->last_push = now;
  if (!playing || !route->window_start || gap > 500 * G_TIME_SPAN_MILLISECOND) {
    route->window_start = playing ? now : 0;
    route->window_media = 0;
    route->slow_windows = 0;
    return FALSE;
  }
  route->window_media += gst_util_uint64_scale (size, GST_SECOND, CARRIER_BYTES_PER_SECOND);
  if (route->window_media < PACE_WINDOW)
    return FALSE;
  guint64 took = (guint64) (now - route->window_start) * GST_USECOND;
  if (took > 3 * route->window_media) {
    route->slow_windows++;
    gchar *lg = lg_output ();
    note ("carrier slow: %" G_GUINT64_FORMAT " ms of audio took %" G_GUINT64_FORMAT " ms, sending silence%s",
        route->window_media / GST_MSECOND, took / GST_MSECOND, lg);
    g_free (lg);
  } else {
    route->slow_windows = 0;
  }
  route->window_start = now;
  route->window_media = 0;
  return route->slow_windows >= 3;
}

static GstPadProbeReturn event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GST_LOG_OBJECT (route->element, "lgroute: event %s (configured=%d unmixed=%d)",
      GST_EVENT_TYPE_NAME (event), route->configured, route->unmixed);
  if (GST_EVENT_TYPE (event) == GST_EVENT_STREAM_START) {
    const gchar *id = NULL;
    gst_event_parse_stream_start (event, &id);
    if (!id)
      id = "";
    if (!route->configured) {
      g_strlcpy (route->stream_id, id, sizeof route->stream_id);
    } else if (route->stream_id[0] && !g_strcmp0 (route->stream_id, id)) {
      /* LG's sink rebuilds its renderer only when the id changes. */
      GST_INFO_OBJECT (route->element, "lgroute: repeated STREAM_START \"%s\", route kept", id);
    } else {
      /* The sink restarts the renderer (close, open, config), which re-arms its
       * FIRST event and drops our converter setup: redo both for this stream.
       * Carrier data waits for the new caps and a fresh mixer reading. */
      GST_INFO_OBJECT (route->element, "lgroute: stream id \"%s\" -> \"%s\", renderer "
          "will restart, redoing route setup", route->stream_id, id);
      g_strlcpy (route->stream_id, id, sizeof route->stream_id);
      route->configured = route->first = route->feeding = route->unmixed = FALSE;
      route->next_check = route->mixed_since = 0;
      restart_pace (route);
      /* Sticky caps outlive STREAM_START and identical caps are never sent
       * again, so after a track switch the carrier already on the pad is this
       * stream's caps. The flag must be set before LG's sink reopens. */
      if (pad_has_carrier (pad)) {
        route->first = TRUE;
        set_pcm (route, TRUE);
      }
      /* The end of the id tells tracks and our own restarts apart. */
      note ("new stream \"%.60s\", route setup %s", id + (strlen (id) > 60 ? strlen (id) - 60 : 0),
          route->first ? "redone on the next buffer" : "waits for caps");
    }
    return GST_PAD_PROBE_OK;
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    /* Seeks and resumes flush; the next carrier buffer must see LG's current mixer state. */
    GST_INFO_OBJECT (route->element, "lgroute: FLUSH_STOP, mixer state re-read before next carrier");
    route->next_check = 0;
    route->unmixed = FALSE;
    restart_pace (route);
    /* A resume seeks right after the route starts. Every resume a tester logged
     * flushed within 30 ms of the setup and then stayed silent or played noise,
     * while seeks later in playback kept working: decode instead. */
    if (route->configured && g_get_monotonic_time () - route->configured_at < G_TIME_SPAN_SECOND) {
      if (!restart_wanted (route, "seek right after the start (resume)"))
        route_fail (route, "seek right after the start (resume)");
    } else
      note ("flush, converter %s %s", route->converter, converter_present (route) ? "present" : "gone");
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
    GstCaps *caps = NULL, *carrier = lgroute_carrier_caps ();
    gst_event_parse_caps (event, &caps);
    gboolean is_carrier = gst_caps_is_equal (caps, carrier);
    gst_caps_unref (carrier);
    if (!is_carrier) {
      /* The earlier stream's own caps, queued like its buffers. */
      if (route->waiting)
        return GST_PAD_PROBE_OK;
      route_fail (route, "HD transport replaced by another format");
      g_mutex_lock (&route_lock);
      route->events = 0;   /* this probe removes itself */
      g_mutex_unlock (&route_lock);
      lgroute_disarm (route);
      return GST_PAD_PROBE_REMOVE;
    }
    route->waiting = FALSE;
    if (route->configured) {
      /* Identical caps do not make LG's sink reopen the renderer. */
      GST_INFO_OBJECT (route->element, "lgroute: repeated carrier CAPS, route kept");
      return GST_PAD_PROBE_OK;
    }
    GST_INFO_OBJECT (route->element, "lgroute: carrier CAPS, renderer setup pending");
    route->first = TRUE;
    route->feeding = FALSE;
    restart_pace (route);
    set_pcm (route, TRUE);
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  (void) pad;
  if (route->failed)
    return GST_PAD_PROBE_DROP;
  if (route->configured && !route->restart_pending && !converter_present (route)
      && !restart_wanted (route, "LG rebuilt its audio path")) {
    route_fail (route, "LG rebuilt its audio path without our setup");
    return GST_PAD_PROBE_DROP;
  }
  if (route->restart_pending) {
    if (!sink_playing (route) || g_get_monotonic_time () < route->restart_after) {
      /* The restart starts LG's renderer at once, so it waits for playback;
       * until then the receiver gets silence, typed PCM like any carrier. */
      GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
      if (!buffer)
        return GST_PAD_PROBE_DROP;
      GST_PAD_PROBE_INFO_DATA (info) = buffer;
      GST_BUFFER_FLAG_UNSET (buffer, ES_FLAG);
      return silence (info);
    }
    if (!restart_renderer (route)) {
      route_fail (route, "LG's renderer did not restart");
      return GST_PAD_PROBE_DROP;
    }
  }
  if (!route->configured) {
    if (!route->first) {
      /* The route arms on the new track's first frame, while the previous
       * track's buffers are still queued in LG's playsink (its aqueue) ahead
       * of our caps: until those arrive, a buffer is the old track's and
       * passes untouched. A tester's G5 logged "armed" and this failure
       * 8-22 ms apart on every switch back to TrueHD and on a DTS track switch. */
      if (route->waiting) {
        if (!route->draining)
          note ("earlier stream still draining, passed through");
        route->draining = TRUE;
        return GST_PAD_PROBE_OK;
      }
      route_fail (route, "carrier arrived before renderer caps");
      return GST_PAD_PROBE_DROP;
    }
    route->first = FALSE;
    /* The first ES event must leave with the flag clear. */
    set_pcm (route, FALSE);
    route->configured = configure_converter (route);
    if (!route->configured) {
      route_fail (route, "bypass converter missing, ambiguous, or not configurable");
      return GST_PAD_PROBE_DROP;
    }
    GST_INFO_OBJECT (route->element, "lgroute: bypass converter configured, first carrier buffer");
    route->configured_at = g_get_monotonic_time ();
    note ("configured converter %s%s", route->converter, route->own ? " of this renderer" : "");
    /* After a track switch LG's renderer keeps the old track's pace: a G3 and
     * another tester's TV played every such carrier four times too slowly. Restart it, as
     * after a resume. */
    if (route->draining) {
      route->draining = FALSE;
      /* LG has restarted the renderer itself for the new track a moment ago
       * and is still rewiring its output. A tester's TV, with ours following
       * 10-20 ms later, twice hung in the restart with LG's audio driver
       * locked (TV audio gone until a reboot) and once found no converter. */
      if (restart_wanted (route, "track switch"))
        route->restart_after = route->configured_at + RESTART_SETTLE;
    }
    route->leased = app_lease (route->lease);
    if (!route->leased) {
      route_fail (route, "earc_route did not take the lease");
      return GST_PAD_PROBE_DROP;
    }
    note ("earc_route holds LG's bypass for %s", route->resource);
  } else {
    GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
    if (!buffer)
      return GST_PAD_PROBE_DROP;
    GST_PAD_PROBE_INFO_DATA (info) = buffer;
    GST_BUFFER_FLAG_UNSET (buffer, ES_FLAG);
    if (!route->feeding) {
      set_pcm (route, TRUE);
      route->feeding = TRUE;
    }
  }
  gint64 now = g_get_monotonic_time ();
  gboolean due = now >= route->next_check, was_unmixed = route->unmixed;
  /* Read on every buffer: LG can switch eARC back to mixed at any time, and a mixed
   * eARC plays the carrier as noise. It is one cheap ALSA ioctl read. */
  char why[LGROUTE_WHY] = "sound output is not a receiver";
  int unmixed = lgroute_control_receiver () ? lgroute_control_unmixed_why (why) : -1;
  /* LG's mixer word is what LG asked for, not what the kernel did. A tester's
   * TV log had LG's mixer taking the carrier as 8-channel PCM, its queue full,
   * while the word read unmixed: white noise at the receiver. The kernel's
   * status names the route it took. It counts once this TV has shown its
   * bypass there, so a kernel that words it differently changes nothing. */
  if (unmixed > 0 && (due || !route->unmixed)) {
    int bypassing = lg_bypassing ();
    if (bypassing > 0 && access (BYPASS_SEEN, F_OK))
      g_file_set_contents (BYPASS_SEEN, "", 0, NULL);
    route->in_mixer = bypassing == 0 && !access (BYPASS_SEEN, F_OK);
  }
  gboolean in_mixer = unmixed > 0 && route->in_mixer;
  if (in_mixer)
    unmixed = 0;
  /* The sink takes no buffers while it prerolls or is paused, so that time says
   * nothing about LG's mixer. A G2 read "mixed" on the preroll buffer and gave
   * up on the next one, 1.7 s later, without the second it is meant to wait. */
  if (route->mixed_since && now - route->last_push > 500 * G_TIME_SPAN_MILLISECOND)
    route->mixed_since = 0;
  /* An unavailable reading gets the same second: a tester's TrueHD route failed
   * on one, at the moment LG switched tracks. */
  if (unmixed > 0)
    route->mixed_since = 0;
  else if (!route->mixed_since)
    route->mixed_since = now;
  route->unmixed = unmixed > 0;
  if (route->unmixed != was_unmixed) {
    GST_INFO_OBJECT (route->element, "lgroute: eARC now %s (reading %d)",
        route->unmixed ? "unmixed, carrier passes" : "mixed or unavailable, sending silence", unmixed);
    gchar *lg = lg_output ();
    if (unmixed < 0)
      note ("eARC Pass Through unavailable (%s), sending silence%s", why, lg);
    else
      note ("eARC %s%s", route->unmixed ? "unmixed, carrier passes" : in_mixer
          ? "unmixed but LG's mixer has the carrier, sending silence" : "mixed, sending silence", lg);
    g_free (lg);
  }
  /* LG's policy rewrites can mix briefly; only a lasting mix means TV sound overlays the carrier. */
  if (route->mixed_since && now - route->mixed_since >= G_TIME_SPAN_SECOND) {
    if (unmixed < 0)
      g_snprintf (route->unavailable, sizeof route->unavailable, "eARC Pass Through unavailable (%s)", why);
    route_fail (route, unmixed < 0 ? route->unavailable
        : in_mixer ? "LG keeps the carrier in its mixer" : "LG keeps eARC mixing on");
    return GST_PAD_PROBE_DROP;
  }
  if (carrier_slow (route, gst_buffer_get_size (GST_PAD_PROBE_INFO_BUFFER (info)), now)) {
    route_fail (route, "LG plays the carrier too slowly");
    return GST_PAD_PROBE_DROP;
  }
  if (due) {
    route->next_check = now + 250 * G_TIME_SPAN_MILLISECOND;
    if (!app_lease (route->lease)) {
      route->leased = FALSE;
      route_fail (route, "earc_route lease lost");
      return GST_PAD_PROBE_DROP;
    }
  }
  /* Mixed eARC would play the carrier as noise, and so does a carrier LG plays
   * at the wrong speed (a tester heard seconds of it before the third slow
   * window): send silence until LG unmixes and the pace is back. */
  return route->unmixed && !route->slow_windows ? GST_PAD_PROBE_OK : silence (info);
}

/* The factories of the sinks under top, for the route log. */
static gchar *sink_names (GstObject *top)
{
  GString *names = g_string_new (NULL);
  GstIterator *iterator = GST_IS_BIN (top) ? gst_bin_iterate_recurse (GST_BIN (top)) : NULL;
  GValue value = G_VALUE_INIT;
  GstIteratorResult result;
  while (iterator && (result = gst_iterator_next (iterator, &value)) != GST_ITERATOR_DONE
      && result != GST_ITERATOR_ERROR) {
    if (result == GST_ITERATOR_RESYNC) {
      gst_iterator_resync (iterator);
      g_string_truncate (names, 0);
      continue;
    }
    GstElement *element = g_value_get_object (&value);
    GstElementFactory *factory = gst_element_get_factory (element);
    if (!GST_IS_BIN (element) && GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SINK))
      g_string_append_printf (names, " %s", factory ? GST_OBJECT_NAME (factory) : "?");
    g_value_reset (&value);
  }
  g_value_unset (&value);
  if (iterator) gst_iterator_free (iterator);
  return g_string_free (names, FALSE);
}

void lgroute_init (LgRoute *route, GstElement *element)
{
  static gsize category;
  if (g_once_init_enter (&category)) {
    GST_DEBUG_CATEGORY_INIT (lgroute_debug, "dtsdec-lgroute", 0, "LG eARC HD transport");
    g_once_init_leave (&category, 1);
  }
  memset (route, 0, sizeof *route);
  route->element = element;
}

/* The first verified LG sink under top; count the verified ones, total every audiosink. */
static GstElement *find_sink (GstObject *top, unsigned *count, unsigned *total)
{
  GstElement *sink = NULL;
  *count = *total = 0;
  if (!GST_IS_BIN (top))
    return NULL;
  GstIterator *iterator = gst_bin_iterate_all_by_element_factory_name (GST_BIN (top), "audiosink");
  GValue value = G_VALUE_INIT;
  for (GstIteratorResult result; (result = gst_iterator_next (iterator, &value)) != GST_ITERATOR_DONE
      && result != GST_ITERATOR_ERROR;) {
    if (result == GST_ITERATOR_RESYNC) {
      gst_iterator_resync (iterator);
      gst_clear_object (&sink);
      *count = *total = 0;
      continue;
    }
    GstElement *element = g_value_get_object (&value);
    ++*total;
    if (verified_sink (element) && !(*count)++)
      sink = gst_object_ref (element);
    g_value_reset (&value);
  }
  g_value_unset (&value);
  gst_iterator_free (iterator);
  return sink;
}

/* TRUE once an LG audio sink under top has caps: the pipeline has played a
 * track, so this first frame is a track switch and not the start of a file. */
static gboolean sink_has_caps (GstObject *top)
{
  gboolean has = FALSE;
  if (!GST_IS_BIN (top))
    return FALSE;
  GstIterator *iterator = gst_bin_iterate_all_by_element_factory_name (GST_BIN (top), "audiosink");
  GValue value = G_VALUE_INIT;
  for (GstIteratorResult result; !has && (result = gst_iterator_next (iterator, &value)) != GST_ITERATOR_DONE
      && result != GST_ITERATOR_ERROR;) {
    if (result == GST_ITERATOR_RESYNC) {
      gst_iterator_resync (iterator);
      continue;
    }
    GstPad *pad = gst_element_get_static_pad (g_value_get_object (&value), "sink");
    has = pad && gst_pad_has_current_caps (pad);
    if (pad) gst_object_unref (pad);
    g_value_reset (&value);
  }
  g_value_unset (&value);
  gst_iterator_free (iterator);
  return has;
}

/* A tester's TrueHD to DTS switch read eARC unavailable once, and the DTS track
 * stayed core only and silent; the end-of-session report read it healthy. LG
 * rewires its eARC output at a track switch while the earlier track drains, so
 * there it is read again for up to 500 ms. A first play does not wait. */
static int earc_after_switch (LgRoute *route, char why[LGROUTE_WHY])
{
  GstObject *top = gst_object_ref (route->element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  gboolean switching = GST_IS_ELEMENT (top) && GST_STATE_TARGET (top) >= GST_STATE_PAUSED
      && sink_has_caps (top);
  gst_object_unref (top);
  if (!switching)
    return -1;
  gchar first[LGROUTE_WHY];
  g_strlcpy (first, why, sizeof first);
  GstPad *input = gst_element_get_static_pad (route->element, "sink");
  gint64 start = g_get_monotonic_time (), deadline = start + 500 * G_TIME_SPAN_MILLISECOND, now;
  int unmixed = -1;
  while (unmixed < 0 && (now = g_get_monotonic_time ()) < deadline
      && !(input && GST_PAD_IS_FLUSHING (input))) {
    g_usleep (MIN (50 * G_TIME_SPAN_MILLISECOND, deadline - now));
    unmixed = lgroute_control_unmixed_why (why);
  }
  gst_clear_object (&input);
  gchar *then = unmixed < 0 ? g_strdup_printf ("unavailable (%s)", why) : g_strdup (unmixed ? "unmixed" : "mixed");
  note ("track switch: eARC Pass Through unavailable (%s), read again for %" G_GINT64_FORMAT " ms: %s",
      first, (g_get_monotonic_time () - start) / G_TIME_SPAN_MILLISECOND, then);
  g_free (then);
  return unmixed;
}

gboolean lgroute_arm (LgRoute *route)
{
  char why[LGROUTE_WHY];
  g_mutex_lock (&route_lock);
  gboolean armed = route->sink != NULL;
  g_mutex_unlock (&route_lock);
  if (armed)
    return !route->failed;
  if (route->failed) {
    note ("not armed: gave up earlier on this track (%s)", route->reason ? route->reason : "?");
    return FALSE;
  }
  if (!lgroute_control_receiver ()) {
    note ("not armed: sound output is not a receiver");
    return FALSE;
  }
  if (access (LGROUTE_APP_ROUTE, W_OK)) {
    note ("not armed: earc_route is not loaded");
    return FALSE;
  }
  if (lgroute_control_unmixed_why (why) < 0 && earc_after_switch (route, why) < 0) {
    note ("not armed: eARC Pass Through unavailable (%s)", why);
    return FALSE;
  }
  GstElement *sink = NULL;
  GstObject *top = gst_object_ref (route->element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  unsigned count = 0, total = 0;
  /* A first frame can arrive before LG's playsink has built its audio chain: a
   * tester's DTS:X opened as core DTS ("0 audiosink elements; playbin3-2 is
   * READY, going PAUSED"), and on a G5 the sink came 100-300 ms later. Only
   * while the pipeline starts or runs: in one going to NULL the sink is gone
   * for good, and a tester's log had this wait hold a closing file for 1.75 s. */
  gint64 deadline = g_get_monotonic_time () + LGROUTE_SINK_WAIT;
  GstPad *input = gst_element_get_static_pad (route->element, "sink");
  gboolean starting = GST_IS_ELEMENT (top) && GST_STATE_TARGET (top) >= GST_STATE_PAUSED;
  while (!(sink = find_sink (top, &count, &total)) && !total && starting
      && g_get_monotonic_time () < deadline && !(input && GST_PAD_IS_FLUSHING (input)))
    g_usleep (50 * G_TIME_SPAN_MILLISECOND);
  gst_clear_object (&input);
  if (count != 1 || !sink) {
    route->reason = "no unique verified LG sink in pipeline";
    /* Testers' opens logged 0 verified sinks, once in a pipeline that armed on
     * a later track. What decides the fix: LG's sink present but not the
     * verified build, no audio sink yet (and the pipeline starting, playing or
     * stopping), or another sink in its place. */
    gchar *sinks = sink_names (top);
    note ("not armed: %u verified LG sinks in the pipeline (%u audiosink elements; %s is %s, going %s; sinks:%s)",
        count, total, GST_OBJECT_NAME (top),
        GST_IS_ELEMENT (top) ? gst_element_state_get_name (GST_STATE (top)) : "?",
        GST_IS_ELEMENT (top) ? gst_element_state_get_name (GST_STATE_PENDING (top)) : "?",
        sinks[0] ? sinks : " none");
    g_free (sinks);
    gst_object_unref (top);
    gst_clear_object (&sink);
    return FALSE;
  }
  gst_object_unref (top);
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  if (!pad) { gst_object_unref (sink); return FALSE; }
  gchar resource[sizeof route->resource] = "";
  GObjectClass *klass = G_OBJECT_GET_CLASS (sink);
  if (g_object_class_find_property (klass, "index") && g_object_class_find_property (klass, "mixer")) {
    guint index = 0;
    gboolean mixer = FALSE;
    g_object_get (sink, "index", &index, "mixer", &mixer, NULL);
    g_snprintf (resource, sizeof resource, "%s%u", mixer ? "AMIXER" : "ADEC", index);
    route->lease = !mixer && index <= 1 ? (gint) index + 1 : 0;
  }
  if (!route->lease) {
    note ("not armed: %s has no earc_route lease", resource[0] ? resource : "LG's sink");
    gst_object_unref (pad);
    gst_object_unref (sink);
    return FALSE;
  }
  /* The probes run at once on sticky events, so the route is published first.
   * Until our caps reach the sink, what arrives there is the earlier stream's. */
  route->waiting = TRUE;
  g_mutex_lock (&route_lock);
  route->sink = sink;
  route->sinkpad = pad;
  g_strlcpy (route->resource, resource, sizeof route->resource);
  g_mutex_unlock (&route_lock);
  gulong events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, route, NULL);
  gulong buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL);
  g_mutex_lock (&route_lock);
  route->events = events;
  route->buffers = buffers;
  g_mutex_unlock (&route_lock);
  /* A replay re-arms on a pad that already holds the carrier caps and may not resend them. */
  if (pad_has_carrier (pad)) {
    route->first = TRUE;
    route->feeding = route->waiting = FALSE;
    set_pcm (route, TRUE);
  }
  /* What LG's renderer is set up for when a track switch arms the route. */
  GstCaps *playing = gst_pad_get_current_caps (pad);
  gchar *was = playing && !route->first ? gst_caps_to_string (playing) : NULL;
  note ("armed on %s%s%.200s", resource[0] ? resource : "LG sink", was ? ", sink has " : "", was ? was : "");
  g_free (was);
  if (playing) gst_caps_unref (playing);
  return TRUE;
}

/* Callable from several threads at once (the sink's and the decoder's streaming
 * threads, an old decoder's teardown): the first caller takes everything, so
 * the probes are removed and the lease released exactly once. */
void lgroute_disarm (LgRoute *route)
{
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink;
  GstPad *pad = route->sinkpad;
  gulong events = route->events, buffers = route->buffers;
  route->sink = NULL;
  route->sinkpad = NULL;
  route->events = route->buffers = 0;
  route->mixed_since = 0;
  route->first = route->feeding = route->configured = route->unmixed = FALSE;
  route->restart_pending = route->waiting = route->draining = route->in_mixer = FALSE;
  route->restarts = 0;
  if (route->leased)
    app_lease (0);
  route->lease = 0;
  route->leased = FALSE;
  route->restart_after = 0;
  route->stream_id[0] = 0;
  route->resource[0] = 0;
  restart_pace (route);
  g_mutex_unlock (&route_lock);
  if (sink)
    note ("disarmed");
  if (pad) {
    if (events) gst_pad_remove_probe (pad, events);
    if (buffers) gst_pad_remove_probe (pad, buffers);
  }
  if (sink)
    g_object_set (sink, "pcm-multi-ch", FALSE, NULL);
  gst_clear_object (&pad);
  gst_clear_object (&sink);
}
