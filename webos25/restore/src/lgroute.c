/* LG-managed PCM bypass transport. The first ES event survives the PCM-only
 * RTS/converter; lipsync retypes subsequent carrier data as ES.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute.h"
#include "lgroute-graph.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MODULE_GRAPH "/proc/aaudio/dot_module_connection"
#define SINK_PLUGIN "/usr/lib/gstreamer-1.0/libgstlgaudiosink.so"
#define ES_FLAG (1u << 21)

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

/* PCMCVT_CMD_SET_ALLFMT: 192 kHz, 8 ch, 16-bit in and out; LG's gain is left alone. */
static gboolean configure_converter (void)
{
  static const char cmd[] = "0x50020020 192000 8 16 0 0 192000 8 16\n";
  gchar *graph = NULL, uid[9];
  if (!g_file_get_contents (MODULE_GRAPH, &graph, NULL, NULL))
    return FALSE;
  gboolean ok = lgroute_bypass_converter (graph, uid);
  g_free (graph);
  if (!ok)
    return FALSE;
  gchar *path = g_strdup_printf ("/proc/aaudio/instances/%s/cmd", uid);
  int fd = open (path, O_WRONLY | O_CLOEXEC);
  g_free (path);
  ok = fd >= 0 && write (fd, cmd, sizeof cmd - 1) == (ssize_t) (sizeof cmd - 1);
  if (fd >= 0) close (fd);
  return ok;
}

static GstPadProbeReturn event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  (void) pad;
  if (GST_EVENT_TYPE (event) == GST_EVENT_STREAM_START && route->configured) {
    route->failed = TRUE;
    route->reason = "stream changed during HD transport";
    return GST_PAD_PROBE_OK;
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP)
    route->next_check = 0;
  if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
    GstCaps *caps = NULL, *carrier = lgroute_carrier_caps ();
    gst_event_parse_caps (event, &caps);
    gboolean is_carrier = gst_caps_is_equal (caps, carrier);
    gst_caps_unref (carrier);
    if (!is_carrier) {
      route->failed = TRUE;
      route->reason = "HD transport replaced by another format";
      route->events = 0;
      lgroute_disarm (route);
      return GST_PAD_PROBE_REMOVE;
    }
    if (route->configured) {
      route->failed = TRUE;
      route->reason = "renderer caps changed during HD transport";
      return GST_PAD_PROBE_DROP;
    }
    route->first = TRUE;
    route->feeding = FALSE;
    g_object_set (route->sink, "pcm-multi-ch", TRUE, NULL);
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  (void) pad;
  if (!lgroute_control_receiver ()) {
    route->failed = TRUE;
    route->reason = "receiver or HD opt-in disappeared";
  }
  if (route->failed)
    return GST_PAD_PROBE_DROP;
  if (!route->configured) {
    if (!route->first) {
      route->failed = TRUE;
      route->reason = "carrier arrived before renderer caps";
      return GST_PAD_PROBE_DROP;
    }
    route->first = FALSE;
    /* The first ES event must leave with the flag clear. */
    g_object_set (route->sink, "pcm-multi-ch", FALSE, NULL);
    route->configured = configure_converter ();
    if (!route->configured) {
      route->failed = TRUE;
      route->reason = "bypass converter missing, ambiguous, or not configurable";
      return GST_PAD_PROBE_DROP;
    }
  } else {
    GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
    if (!buffer)
      return GST_PAD_PROBE_DROP;
    GST_PAD_PROBE_INFO_DATA (info) = buffer;
    GST_BUFFER_FLAG_UNSET (buffer, ES_FLAG);
    if (!route->feeding) {
      g_object_set (route->sink, "pcm-multi-ch", TRUE, NULL);
      route->feeding = TRUE;
    }
  }
  gint64 now = g_get_monotonic_time ();
  if (now >= route->next_check) {
    if (lgroute_control_apply (&route->control) < 0) {
      route->failed = TRUE;
      route->reason = "unmixed eARC unavailable or output policy changed";
      return GST_PAD_PROBE_DROP;
    }
    route->next_check = now + 250 * G_TIME_SPAN_MILLISECOND;
  }
  return GST_PAD_PROBE_OK;
}

void lgroute_init (LgRoute *route, GstElement *element)
{
  memset (route, 0, sizeof *route);
  route->element = element;
  route->control.fd = route->control.lock_fd = -1;
}

gboolean lgroute_arm (LgRoute *route)
{
  if (route->sink)
    return !route->failed;
  if (route->failed || !lgroute_control_receiver ()
      || lgroute_control_open (&route->control) < 0)
    return FALSE;
  GstObject *top = gst_object_ref (route->element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  unsigned count = 0;
  if (GST_IS_BIN (top)) {
    GstIterator *iterator = gst_bin_iterate_recurse (GST_BIN (top));
    GValue value = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
      switch (gst_iterator_next (iterator, &value)) {
        case GST_ITERATOR_OK: {
          GstElement *element = g_value_get_object (&value);
          if (verified_sink (element)) {
            if (!count++) route->sink = gst_object_ref (element);
          }
          g_value_reset (&value);
          break;
        }
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync (iterator);
          gst_clear_object (&route->sink);
          count = 0;
          break;
        default: done = TRUE; break;
      }
    }
    g_value_unset (&value);
    gst_iterator_free (iterator);
  }
  gst_object_unref (top);
  if (count != 1 || !route->sink) {
    route->reason = "no unique verified LG sink in pipeline";
    lgroute_disarm (route);
    return FALSE;
  }
  route->sinkpad = gst_element_get_static_pad (route->sink, "sink");
  if (!route->sinkpad) { lgroute_disarm (route); return FALSE; }
  route->events = gst_pad_add_probe (route->sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, event_probe, route, NULL);
  route->buffers = gst_pad_add_probe (route->sinkpad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL);
  return TRUE;
}

void lgroute_disarm (LgRoute *route)
{
  if (route->sinkpad) {
    if (route->events) gst_pad_remove_probe (route->sinkpad, route->events);
    if (route->buffers) gst_pad_remove_probe (route->sinkpad, route->buffers);
  }
  if (route->sink)
    g_object_set (route->sink, "pcm-multi-ch", FALSE, NULL);
  lgroute_control_release (&route->control);
  gst_clear_object (&route->sinkpad);
  gst_clear_object (&route->sink);
  route->events = route->buffers = 0;
  route->first = route->feeding = route->configured = FALSE;
}
