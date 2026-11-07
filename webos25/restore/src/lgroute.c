/* LG-managed PCM bypass transport. The first ES event survives the PCM-only
 * RTS/converter; lipsync retypes subsequent carrier data as ES.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "lgroute.h"
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
  GObjectClass *klass = G_OBJECT_GET_CLASS (sink);
  GstPlugin *plugin;
  gboolean ok;
  if (!factory || strcmp (GST_OBJECT_NAME (factory), "audiosink")
      || !g_object_class_find_property (klass, "pcm-multi-ch")
      || !g_object_class_find_property (klass, "fade")
      || !g_object_class_find_property (klass, "fade-volume"))
    return FALSE;
  plugin = gst_plugin_feature_get_plugin (GST_PLUGIN_FEATURE (factory));
  if (!plugin)
    return FALSE;
  ok = !g_strcmp0 (gst_plugin_get_filename (plugin), SINK_PLUGIN)
      && access (LGROUTE_SINK_OK, F_OK) == 0;
  gst_object_unref (plugin);
  return ok;
}

/* LG's fade reaches only the main renderer gain; the bypass path has none. */
static void main_volume (LgRoute *route, gint volume)
{
  GstStructure *fade = gst_structure_new ("fade", "target-volume", G_TYPE_INT, volume,
      "duration-in-ms", G_TYPE_INT, 0, "fade", G_TYPE_STRING, "Linear", NULL);
  g_object_set (route->sink, "fade", fade, NULL);
  gst_structure_free (fade);
  route->muted = volume == 0;
}

/* Re-mute when the player starts its own fade on the main renderer. */
static void main_mute (LgRoute *route)
{
  gint volume = -1;
  if (route->muted)
    g_object_get (route->sink, "fade-volume", &volume, NULL);
  if (volume != 0)
    main_volume (route, 0);
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

/* Only use a converter created by this CAPS transaction. Ambiguity refuses HD. */
static gboolean configure_converter (LgRoute *route, gchar **lines)
{
  gchar candidate[9] = "";
  unsigned count = 0;
  for (unsigned i = 0; lines[i]; i++) {
    char rts[9], source[9], hash[17];
    if (sscanf (lines[i], " rts_%8[0-9a-f]_%16[0-9a-f]_out0 -> srcbin_%8[0-9a-f]", rts, hash, source) != 3)
      continue;
    gchar *node = g_strdup_printf ("srcbin_%s", source);
    if (!strstr (route->before_graph, node)) {
      gchar *edge = g_strdup_printf ("srcbin_%s_", source);
      gboolean connected = FALSE;
      for (unsigned j = 0; lines[j]; j++)
        if (strstr (lines[j], edge) && strstr (lines[j], "_out0 -> lipsync_"))
          connected = TRUE;
      g_free (edge);
      if (connected && strcmp (candidate, source)) {
        g_strlcpy (candidate, source, sizeof candidate);
        count++;
      }
    }
    g_free (node);
  }
  if (count != 1)
    return FALSE;
  gchar *path = g_strdup_printf ("/proc/aaudio/instances/%s/prop", candidate);
  int fd = open (path, O_WRONLY | O_CLOEXEC);
  static const char properties[] =
      "in-sampling-rate 192000\n"
      "in-channels 8\n"
      "in-bit-per-sample 16\n"
      "endian 0\n"
      "sign 0\n"
      "out-sampling-rate 192000\n"
      "out-channels 8\n"
      "out-bit-per-sample 16\n"
      "gain-enable false\n";
  gboolean ok = fd >= 0
      && write (fd, properties, sizeof properties - 1) == sizeof properties - 1;
  if (fd >= 0) close (fd);
  g_free (path);
  return ok;
}

static GstPadProbeReturn event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  (void) pad;
  if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
    /* A replacement renderer cannot inherit the old renderer's mute target. */
    if (route->configured) {
      route->failed = TRUE;
      route->reason = "renderer caps changed during HD transport";
      return GST_PAD_PROBE_DROP;
    }
    g_clear_pointer (&route->before_graph, g_free);
    if (!g_file_get_contents (MODULE_GRAPH, &route->before_graph, NULL, NULL)) {
      route->failed = TRUE;
      route->reason = "cannot snapshot renderer graph";
      return GST_PAD_PROBE_DROP;
    }
    route->first = TRUE;
    route->feeding = FALSE;
    /* pcm-multi-ch also feeds the carrier to the main path; mute it first. */
    main_mute (route);
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
  if (!route->first && !route->configured) {
    route->failed = TRUE;
    route->reason = "carrier arrived before renderer caps";
    return GST_PAD_PROBE_DROP;
  }
  if (route->first) {
    route->first = FALSE;
    gchar *graph = NULL;
    if (g_file_get_contents (MODULE_GRAPH, &graph, NULL, NULL)) {
      gchar **lines = g_strsplit (graph, "\n", -1);
      route->configured = configure_converter (route, lines);
      g_strfreev (lines);
      g_free (graph);
    }
    if (!route->configured) {
      route->failed = TRUE;
      route->reason = "new bypass converter missing, ambiguous, or not configurable";
      return GST_PAD_PROBE_DROP;
    }
    g_clear_pointer (&route->before_graph, g_free);
    g_object_set (route->sink, "pcm-multi-ch", FALSE, NULL);
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
    main_mute (route);
    if (lgroute_control_apply (&route->control) < 0) {
      route->failed = TRUE;
      route->reason = "cannot request unmixed eARC";
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
  if (route->sink) {
    g_object_set (route->sink, "pcm-multi-ch", FALSE, NULL);
    if (route->muted)
      main_volume (route, 100);
  }
  lgroute_control_release (&route->control);
  gst_clear_object (&route->sinkpad);
  gst_clear_object (&route->sink);
  g_clear_pointer (&route->before_graph, g_free);
  route->events = route->buffers = 0;
  route->muted = FALSE;
  route->feeding = route->configured = FALSE;
}
