/* Kernel 4.4 (webOS 6) HD route for dtsdec: lgroute.h's API over the 4.4 earc_route module.
 *
 * webOS 6's lxaudiosink has no ES carrier caps and no /proc/aaudio, so the carrier is
 * plain 192 kHz/8ch/16-bit PCM with LG's PCM bypass on (pcm-multi-ch). From the 03.53.45
 * firmware: liblxa opens a second (bypass) renderer and marks it ES only in
 * lxao_open_renderer, and the sink reopens its renderer only on new caps or a new stream
 * id, so pcm-multi-ch goes on before a fresh STREAM_START. The DSP's eARC ES writer
 * (irqout_fill_bc_es8) byte-swaps each 16-bit word, so bursts stay big-endian as on
 * webOS 25. The module, while this lease is held, keeps the leased renderer's converter
 * at that format, clears LG's eARC mix flag and sets the eARC encoded type to MAT.
 * EXPERIMENTAL: built from extracted 4.4 firmware (C1 03.53.45); no TV has played it yet.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef LGROUTE_LEASE
#define LGROUTE_LEASE "/dev/earc_route"
#endif
#define SINK_PLUGIN "/usr/lib/gstreamer-1.0/libgstlxaudiosink.so"
#ifndef LGROUTE_SINK_WAIT
#define LGROUTE_SINK_WAIT (2 * G_TIME_SPAN_SECOND)
#endif
#define ROUTE_LOG LGROUTE_RUN "/lgroute.log"

GST_DEBUG_CATEGORY_STATIC (lgroute_debug);
#define GST_CAT_DEFAULT (lgroute_debug)

static GMutex route_lock;

/* ponytail: lease and log duplicate lgroute.c's; sharing them shifts __LINE__ in the
 * shipped webOS 25 binary, so merge them with its next on-TV rebuild. */
static int app_lease (gint value)
{
  gchar text = '0' + value;
  int fd = open (LGROUTE_LEASE, O_WRONLY | O_CLOEXEC);
  if (fd >= 0 && write (fd, &text, 1) != 1) {
    close (fd);
    fd = -1;
  }
  return fd;
}

void lgroute_log (const gchar *format, ...)
{
  FILE *log = fopen (ROUTE_LOG, "a");
  if (log && fseek (log, 0, SEEK_END) == 0 && ftell (log) > 65536)
    log = freopen (ROUTE_LOG, "w", log);
  if (!log)
    return;
  GDateTime *now = g_date_time_new_now_local ();
  /* Not "%T.%f": %f is GLib 2.66, and the C1's older GLib returned NULL, logging "(null)". */
  gchar *when = g_date_time_format (now, "%T");
  fprintf (log, "%s.%03d ", when, g_date_time_get_microsecond (now) / 1000);
  g_date_time_unref (now);
  va_list args;
  va_start (args, format);
  vfprintf (log, format, args);
  va_end (args);
  fputc ('\n', log);
  g_free (when);
  fclose (log);
}

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
  ok = !g_strcmp0 (gst_plugin_get_filename (plugin), SINK_PLUGIN) && access (LGROUTE_SINK_OK, F_OK) == 0;
  gst_object_unref (plugin);
  return ok;
}

static void route_fail (LgRoute *route, const gchar *reason)
{
  route->failed = TRUE;
  route->reason = reason;
  GST_WARNING_OBJECT (route->element, "lgroute: HD transport failed: %s", reason);
  lgroute_log ("failed, decoding instead: %s", reason);
}

GstCaps *lgroute_carrier_caps (void)
{
  return gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
      "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 192000,
      "channels", G_TYPE_INT, 8, "channel-mask", GST_TYPE_BITMASK, (guint64) 0xc3f, NULL);
}

/* The first verified LG sink under top; count the verified ones, total every audiosink.
 * The hash in sink-ok pins its index and mixer properties. */
static GstElement *find_sink (GstObject *top, unsigned *count, unsigned *total)
{
  GstElement *sink = NULL;
  *count = *total = 0;
  if (!GST_IS_BIN (top))
    return NULL;
  /* gst_bin_iterate_all_by_element_factory_name is 1.18; webOS 6 has 1.16. */
  GstIterator *it = gst_bin_iterate_recurse (GST_BIN (top));
  GValue value = G_VALUE_INIT;
  for (GstIteratorResult r; (r = gst_iterator_next (it, &value)) != GST_ITERATOR_DONE && r != GST_ITERATOR_ERROR;) {
    if (r == GST_ITERATOR_RESYNC) {
      gst_iterator_resync (it);
      g_clear_object (&sink);
      *count = *total = 0;
      continue;
    }
    GstElement *candidate = g_value_get_object (&value);
    GstElementFactory *factory = gst_element_get_factory (candidate);
    if (factory && !strcmp (GST_OBJECT_NAME (factory), "audiosink"))
      ++*total;
    if (verified_sink (candidate) && !(*count)++)
      sink = gst_object_ref (candidate);
    g_value_reset (&value);
  }
  g_value_unset (&value);
  gst_iterator_free (it);
  return sink;
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

/* Silence until LG's eARC is unmixed; a lasting mix gives up, as on webOS 25. */
static GstPadProbeReturn buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  (void) pad;
  if (route->failed)
    return GST_PAD_PROBE_DROP;
  gint64 now = g_get_monotonic_time ();
  char why[LGROUTE_WHY] = "sound output is not a receiver";
  int unmixed = lgroute_control_receiver () ? lgroute_control_unmixed_why (why) : -1;
  if (unmixed == 0 && lgroute_control_unmix () == 0)
    unmixed = lgroute_control_unmixed_why (why);
  if (unmixed > 0)
    route->mixed_since = 0;
  else if (!route->mixed_since)
    route->mixed_since = now;
  if (route->unmixed != (unmixed > 0))
    lgroute_log (unmixed > 0 ? "eARC unmixed, carrier passes" : "eARC %s, sending silence",
        unmixed < 0 ? why : "mixed");
  route->unmixed = unmixed > 0;
  if (route->mixed_since && now - route->mixed_since >= G_TIME_SPAN_SECOND) {
    route_fail (route, unmixed < 0 ? "eARC Pass Through unavailable" : "LG keeps eARC mixing on");
    return GST_PAD_PROBE_DROP;
  }
  if (route->unmixed)
    return GST_PAD_PROBE_OK;
  GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
  if (!buffer)
    return GST_PAD_PROBE_DROP;
  GST_PAD_PROBE_INFO_DATA (info) = buffer;
  gst_buffer_memset (buffer, 0, 0, gst_buffer_get_size (buffer));
  return GST_PAD_PROBE_OK;
}

void lgroute_init (LgRoute *route, GstElement *element)
{
  static gsize category;
  if (g_once_init_enter (&category)) {
    GST_DEBUG_CATEGORY_INIT (lgroute_debug, "dtsdec-lgroute", 0, "LG eARC HD transport, kernel 4.4");
    g_once_init_leave (&category, 1);
  }
  memset (route, 0, sizeof *route);
  route->element = element;
  route->lease_fd = -1;
}

gboolean lgroute_arm (LgRoute *route)
{
  char why[LGROUTE_WHY];
  g_mutex_lock (&route_lock);
  gboolean armed = route->sink != NULL;
  g_mutex_unlock (&route_lock);
  if (armed)
    return !route->failed;
  if (route->failed)
    return FALSE;
  if (!lgroute_control_receiver ()) {
    lgroute_log ("not armed: sound output is not an eARC receiver in Pass Through");
    return FALSE;
  }
  if (lgroute_control_unmixed_why (why) < 0) {
    lgroute_log ("not armed: eARC Pass Through unavailable (%s)", why);
    return FALSE;
  }
  GstObject *top = gst_object_ref (route->element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  /* A C1 tester's DTS-HD MA plays mostly went out as core DTS with no LG sink found.
   * As on webOS 25 (a G5's sink came 100-300 ms after the first frame), wait for an
   * audiosink while the pipeline starts or runs, never while it closes. */
  unsigned count = 0, total = 0;
  gint64 deadline = g_get_monotonic_time () + LGROUTE_SINK_WAIT;
  GstPad *input = gst_element_get_static_pad (route->element, "sink");
  gboolean starting = GST_IS_ELEMENT (top) && GST_STATE_TARGET (top) >= GST_STATE_PAUSED;
  GstElement *sink;
  while (!(sink = find_sink (top, &count, &total)) && !total && starting
      && g_get_monotonic_time () < deadline && !(input && GST_PAD_IS_FLUSHING (input)))
    g_usleep (50 * G_TIME_SPAN_MILLISECOND);
  if (input)
    gst_object_unref (input);
  if (count != 1 || !sink) {
    gchar *sinks = sink_names (top);
    lgroute_log ("not armed: %u verified LG sinks in the pipeline (%u audiosink elements; %s is %s, going %s; sinks:%s)",
        count, total, GST_OBJECT_NAME (top),
        GST_IS_ELEMENT (top) ? gst_element_state_get_name (GST_STATE (top)) : "?",
        GST_IS_ELEMENT (top) ? gst_element_state_get_name (GST_STATE_PENDING (top)) : "?",
        sinks[0] ? sinks : " none");
    g_free (sinks);
    gst_object_unref (top);
    g_clear_object (&sink);
    return FALSE;
  }
  gst_object_unref (top);
  guint index = 0;
  gboolean mixer = FALSE;
  g_object_get (sink, "index", &index, "mixer", &mixer, NULL);
  if (mixer || index > 1) {
    lgroute_log ("not armed: %s%u has no earc_route lease", mixer ? "AMIXER" : "ADEC", index);
    gst_object_unref (sink);
    return FALSE;
  }
  int fd = app_lease ((gint) index + 1);
  if (fd < 0) {
    lgroute_log ("not armed: earc_route did not take the lease");
    gst_object_unref (sink);
    return FALSE;
  }
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  g_mutex_lock (&route_lock);
  route->sink = sink;
  route->sinkpad = pad;
  route->lease_fd = fd;
  g_mutex_unlock (&route_lock);
  route->buffers = pad ? gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL) : 0;
  route->configured = TRUE;
  g_object_set (sink, "pcm-multi-ch", TRUE, NULL);
  /* New caps reopen LG's renderer with the flag; unchanged carrier caps (a replay) do not,
   * so a new stream id reopens it. */
  GstCaps *current = pad ? gst_pad_get_current_caps (pad) : NULL, *carrier = lgroute_carrier_caps ();
  gboolean same = current && gst_caps_is_equal (current, carrier);
  gst_caps_unref (carrier);
  if (current) gst_caps_unref (current);
  if (same) {
    gchar *id = g_strdup_printf ("lgroute-%p-%" G_GINT64_FORMAT, (void *) route, g_get_monotonic_time ());
    gst_pad_send_event (pad, gst_event_new_stream_start (id));
    g_free (id);
  }
  lgroute_log ("armed on ADEC%u: PCM bypass on, earc_route holds the lease", index);
  return TRUE;
}

void lgroute_disarm (LgRoute *route)
{
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink;
  GstPad *pad = route->sinkpad;
  gulong buffers = route->buffers;
  int fd = route->lease_fd;
  route->sink = NULL;
  route->sinkpad = NULL;
  route->buffers = 0;
  route->lease_fd = -1;
  route->configured = route->unmixed = FALSE;
  route->mixed_since = 0;
  g_mutex_unlock (&route_lock);
  if (fd >= 0)
    close (fd);
  if (pad && buffers)
    gst_pad_remove_probe (pad, buffers);
  if (sink) {
    g_object_set (sink, "pcm-multi-ch", FALSE, NULL);
    lgroute_log ("disarmed");
  }
  g_clear_object (&pad);
  g_clear_object (&sink);
}
