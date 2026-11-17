/* webOS 6 (LG C1) HD route for dtsdec: lgroute.h's API over the 4.4 earc_route module.
 *
 * The C1's lxaudiosink has no ES carrier caps and no /proc/aaudio, so the carrier is
 * plain 192 kHz/8ch/16-bit PCM with LG's PCM bypass on (pcm-multi-ch). The module,
 * while this lease is held, keeps the converter of the leased renderer at that format,
 * clears LG's eARC mix flag and sets the eARC encoded type to MAT (LG's HBR route).
 * EXPERIMENTAL: built from the extracted C1 firmware; no C1 has played it yet.
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
#define ROUTE_LOG LGROUTE_RUN "/lgroute.log"

GST_DEBUG_CATEGORY_STATIC (c1route_debug);
#define GST_CAT_DEFAULT (c1route_debug)

static GMutex route_lock;

void lgroute_log (const gchar *format, ...)
{
  FILE *log = fopen (ROUTE_LOG, "a");
  if (log && fseek (log, 0, SEEK_END) == 0 && ftell (log) > 65536)
    log = freopen (ROUTE_LOG, "w", log);
  if (!log)
    return;
  GDateTime *now = g_date_time_new_now_local ();
  gchar *when = g_date_time_format (now, "%T");
  g_date_time_unref (now);
  fprintf (log, "%s c1: ", when);
  va_list args;
  va_start (args, format);
  vfprintf (log, format, args);
  va_end (args);
  fputc ('\n', log);
  g_free (when);
  fclose (log);
}

GstCaps *lgroute_carrier_caps (void)
{
  return gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
      "layout", G_TYPE_STRING, "interleaved", "rate", G_TYPE_INT, 192000,
      "channels", G_TYPE_INT, 8, "channel-mask", GST_TYPE_BITMASK, (guint64) 0xc3f, NULL);
}

/* The boot script hashes SINK_PLUGIN once and writes LGROUTE_SINK_OK on a match. */
static gboolean verified_sink (GstElement *sink)
{
  GstElementFactory *factory = gst_element_get_factory (sink);
  GstPlugin *plugin;
  gboolean ok;
  if (!factory || strcmp (GST_OBJECT_NAME (factory), "audiosink")
      || !g_object_class_find_property (G_OBJECT_GET_CLASS (sink), "pcm-multi-ch")
      || !g_object_class_find_property (G_OBJECT_GET_CLASS (sink), "index")
      || !g_object_class_find_property (G_OBJECT_GET_CLASS (sink), "mixer"))
    return FALSE;
  plugin = gst_plugin_feature_get_plugin (GST_PLUGIN_FEATURE (factory));
  if (!plugin)
    return FALSE;
  ok = !g_strcmp0 (gst_plugin_get_filename (plugin), SINK_PLUGIN) && access (LGROUTE_SINK_OK, F_OK) == 0;
  gst_object_unref (plugin);
  return ok;
}

/* The one verified LG sink in the pipeline, or NULL. */
static GstElement *find_sink (GstElement *element)
{
  GstObject *top = gst_object_ref (element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  GstElement *sink = NULL;
  guint count = 0;
  if (GST_IS_BIN (top)) {
    /* gst_bin_iterate_all_by_element_factory_name is 1.18; the C1 has 1.16. */
    GstIterator *it = gst_bin_iterate_recurse (GST_BIN (top));
    GValue value = G_VALUE_INIT;
    for (GstIteratorResult r; (r = gst_iterator_next (it, &value)) != GST_ITERATOR_DONE && r != GST_ITERATOR_ERROR;) {
      if (r == GST_ITERATOR_RESYNC) {
        gst_iterator_resync (it);
        g_clear_object (&sink);
        count = 0;
        continue;
      }
      GstElement *candidate = g_value_get_object (&value);
      GstElementFactory *factory = gst_element_get_factory (candidate);
      if (factory && !strcmp (GST_OBJECT_NAME (factory), "audiosink") && verified_sink (candidate) && !count++)
        sink = gst_object_ref (candidate);
      g_value_reset (&value);
    }
    g_value_unset (&value);
    gst_iterator_free (it);
  }
  gst_object_unref (top);
  if (count != 1)
    g_clear_object (&sink);
  return sink;
}

static void route_fail (LgRoute *route, const gchar *reason)
{
  route->failed = TRUE;
  route->reason = reason;
  GST_WARNING_OBJECT (route->element, "c1route: HD transport failed: %s", reason);
  lgroute_log ("failed, decoding instead: %s", reason);
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
    GST_DEBUG_CATEGORY_INIT (c1route_debug, "dtsdec-c1route", 0, "LG C1 eARC HD transport");
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
  GstElement *sink = find_sink (route->element);
  if (!sink) {
    lgroute_log ("not armed: no unique verified LG sink in the pipeline");
    return FALSE;
  }
  guint index = 0;
  gboolean mixer = FALSE;
  g_object_get (sink, "index", &index, "mixer", &mixer, NULL);
  if (mixer || index > 1) {
    lgroute_log ("not armed: %s%u has no earc_route lease", mixer ? "AMIXER" : "ADEC", index);
    gst_object_unref (sink);
    return FALSE;
  }
  /* The lease lasts as long as this descriptor: the kernel drops it on close or a crash. */
  gchar value = (gchar) ('1' + index);
  int fd = open (LGROUTE_LEASE, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || write (fd, &value, 1) != 1) {
    lgroute_log ("not armed: earc_route did not take the lease");
    if (fd >= 0) close (fd);
    gst_object_unref (sink);
    return FALSE;
  }
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  g_mutex_lock (&route_lock);
  route->sink = sink;
  route->sinkpad = pad;
  route->lease = (gint) index + 1;
  route->lease_fd = fd;
  g_snprintf (route->resource, sizeof route->resource, "ADEC%u", index);
  g_mutex_unlock (&route_lock);
  route->buffers = pad ? gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL) : 0;
  route->configured = TRUE;
  g_object_set (sink, "pcm-multi-ch", TRUE, NULL);
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
  route->lease = 0;
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
