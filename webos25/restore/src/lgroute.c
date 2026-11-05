/* lgroute.c — LG's own eARC bitstream route, driven from the producer (webOS 25)
 *
 * See lgroute.h and lgroute-policy.h.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gst/gst.h>

#include "lgroute.h"

GST_DEBUG_CATEGORY_EXTERN (hdbitstream_debug);
#define GST_CAT_DEFAULT hdbitstream_debug

#define LG_AUDIOSINK_FACTORY "audiosink"
#define LG_SINK_PROPERTY "pcm-multi-ch"

void
lgroute_log (GstElement * element, const gchar * fmt, ...)
{
  va_list args;
  gchar *msg;
  FILE *f;

  va_start (args, fmt);
  msg = g_strdup_vprintf (fmt, args);
  va_end (args);
  GST_INFO_OBJECT (element, "lgroute: %s", msg);
  if ((f = fopen (LGROUTE_LOG, "a")) != NULL) {
    fprintf (f, "%" G_GINT64_FORMAT " %d %s: %s\n",
        g_get_monotonic_time () / G_TIME_SPAN_MILLISECOND, (gint) getpid (),
        element ? GST_OBJECT_NAME (element) : "-", msg);
    fclose (f);
  }
  g_free (msg);
}

gboolean
lgroute_wanted (void)
{
  gchar *text = NULL, proc[32];
  gboolean ok;
  gint pid = 0;

  if (!g_file_get_contents (LGROUTE_MARKER, &text, NULL, NULL))
    return FALSE;
  ok = lgroute_marker_forces (text);
  g_free (text);
  if (ok)
    return TRUE;
  if (!g_file_get_contents (LGROUTE_SOUND_OUTPUT, &text, NULL, NULL))
    return FALSE;
  ok = lgroute_parse_sound_output (text, &pid);
  g_free (text);
  g_snprintf (proc, sizeof (proc), "/proc/%d", pid);
  return ok && g_file_test (proc, G_FILE_TEST_IS_DIR);
}

GstCaps *
lgroute_carrier_caps (void)
{
  return gst_caps_new_simple ("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "layout", G_TYPE_STRING, "interleaved",
      "rate", G_TYPE_INT, LGROUTE_CARRIER_RATE,
      "channels", G_TYPE_INT, LGROUTE_CARRIER_CHANNELS,
      "channel-mask", GST_TYPE_BITMASK, (guint64) LGROUTE_CARRIER_MASK,
      "acmod", G_TYPE_UINT, (guint) LGROUTE_CARRIER_ACMOD,
      "es", G_TYPE_BOOLEAN, TRUE,
      "codec-str", G_TYPE_STRING, LGROUTE_CARRIER_CODEC_STR,
      "data-rate", G_TYPE_INT, LGROUTE_CARRIER_DATA_RATE,
      "data-channels", G_TYPE_INT, LGROUTE_CARRIER_DATA_CH,
      "data-bps", G_TYPE_INT, LGROUTE_CARRIER_DATA_BPS, NULL);
}

/* Our caps, as they arrive at the sink: the ES description is what the sink
 * reads from them, so that is what identifies them. */
static gboolean
lgroute_caps_are_carrier (GstCaps * caps)
{
  const GstStructure *s;
  gboolean es = FALSE;
  gint data_rate = 0;

  if (caps == NULL || gst_caps_get_size (caps) == 0)
    return FALSE;
  s = gst_caps_get_structure (caps, 0);
  return gst_structure_has_name (s, "audio/x-raw")
      && gst_structure_get_boolean (s, "es", &es) && es
      && gst_structure_get_int (s, "data-rate", &data_rate)
      && data_rate == LGROUTE_CARRIER_DATA_RATE;
}

/* LG's audio sink in the pipeline we are in, or NULL unless there is exactly
 * one. Returns a ref. (Same search as the multich-test branch.) */
static GstElement *
lgroute_find_audiosink (GstElement * element)
{
  GstObject *top = gst_object_ref (GST_OBJECT_CAST (element)), *parent;
  GstElement *found = NULL;
  GValue item = G_VALUE_INIT;
  GstIterator *it;
  gboolean done = FALSE;
  guint count = 0;

  while ((parent = gst_object_get_parent (top)) != NULL) {
    gst_object_unref (top);
    top = parent;
  }
  if (!GST_IS_BIN (top)) {
    gst_object_unref (top);
    return NULL;
  }

  it = gst_bin_iterate_recurse (GST_BIN_CAST (top));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:{
        GstElement *e = g_value_get_object (&item);
        GstElementFactory *f = gst_element_get_factory (e);

        if (f != NULL && !g_strcmp0 (GST_OBJECT_NAME (f), LG_AUDIOSINK_FACTORY)
            && g_object_class_find_property (G_OBJECT_GET_CLASS (e),
                LG_SINK_PROPERTY)) {
          if (count++ == 0)
            found = gst_object_ref (e);
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        gst_clear_object (&found);
        count = 0;
        break;
      default:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  gst_object_unref (top);

  if (count != 1) {
    GST_WARNING_OBJECT (element, "found %u LG audio sinks, need exactly one",
        count);
    gst_clear_object (&found);
  }
  return found;
}

static void
lgroute_apply (LgRoute * route, LgRouteAction act, const gchar * at)
{
  gboolean value = act == LGROUTE_ACT_SET_TRUE, readback = !value;

  if (act == LGROUTE_ACT_NONE)
    return;
  g_object_set (route->sink, LG_SINK_PROPERTY, value, NULL);
  g_object_get (route->sink, LG_SINK_PROPERTY, &readback, NULL);
  lgroute_log (route->element, "%s%s=%s at %s on %s (sink reads back %s)",
      act == LGROUTE_ACT_SET_FALSE && at[0] == 'B' ? "FLIP: " : "",
      LG_SINK_PROPERTY, value ? "TRUE" : "FALSE", at,
      GST_OBJECT_NAME (route->sink), readback ? "TRUE" : "FALSE");
}

static GstPadProbeReturn
lgroute_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  LgRoute *route = user_data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstCaps *caps = NULL;
  gboolean ours;
  gchar *str;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  gst_event_parse_caps (event, &caps);
  ours = lgroute_caps_are_carrier (caps);
  str = gst_caps_to_string (caps);
  lgroute_log (route->element, "CAPS at %s (%s): %s",
      GST_OBJECT_NAME (route->sink), ours ? "carrier" : "not ours", str);
  g_free (str);
  lgroute_apply (route, lgroute_flip_step (&route->flip,
          ours ? LGROUTE_IN_CARRIER_CAPS : LGROUTE_IN_OTHER_CAPS), "CAPS");
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
lgroute_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  LgRoute *route = user_data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  LgRouteAction act = lgroute_flip_step (&route->flip, LGROUTE_IN_BUFFER);

  if (act != LGROUTE_ACT_NONE)
    lgroute_log (route->element, "first buffer at %s: %" G_GSIZE_FORMAT
        " bytes, flags 0x%x (ES bit %s), pts %" GST_TIME_FORMAT,
        GST_OBJECT_NAME (route->sink), gst_buffer_get_size (buffer),
        GST_BUFFER_FLAGS (buffer),
        GST_BUFFER_FLAG_IS_SET (buffer, LGROUTE_BUFFER_FLAG_ES) ? "set" : "CLEAR",
        GST_TIME_ARGS (GST_BUFFER_PTS (buffer)));
  lgroute_apply (route, act, "BUFFER");
  return GST_PAD_PROBE_OK;
}

void
lgroute_init (LgRoute * route, GstElement * element)
{
  memset (route, 0, sizeof (*route));
  route->element = element;
  lgroute_flip_init (&route->flip);
}

gboolean
lgroute_arm (LgRoute * route, const gchar ** why)
{
  gboolean current = FALSE;

  if (route->sink != NULL)
    return TRUE;
  route->sink = lgroute_find_audiosink (route->element);
  if (route->sink == NULL) {
    *why = "no single LG audiosink in this pipeline";
    return FALSE;
  }
  route->sinkpad = gst_element_get_static_pad (route->sink, "sink");
  if (route->sinkpad == NULL) {
    gst_clear_object (&route->sink);
    *why = "LG audiosink has no sink pad";
    return FALSE;
  }
  lgroute_flip_init (&route->flip);
  g_object_get (route->sink, LG_SINK_PROPERTY, &current, NULL);
  lgroute_log (route->element, "armed on %s (%s was %s)",
      GST_OBJECT_NAME (route->sink), LG_SINK_PROPERTY,
      current ? "TRUE" : "FALSE");
  route->event_probe = gst_pad_add_probe (route->sinkpad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, lgroute_event_probe, route, NULL);
  route->buffer_probe = gst_pad_add_probe (route->sinkpad,
      GST_PAD_PROBE_TYPE_BUFFER, lgroute_buffer_probe, route, NULL);
  return TRUE;
}

void
lgroute_disarm (LgRoute * route)
{
  if (route->sink == NULL)
    return;
  gst_pad_remove_probe (route->sinkpad, route->event_probe);
  gst_pad_remove_probe (route->sinkpad, route->buffer_probe);
  if (route->flip.flag)
    lgroute_apply (route, LGROUTE_ACT_SET_FALSE, "disarm");
  lgroute_log (route->element, "disarmed on %s after %d flip(s)",
      GST_OBJECT_NAME (route->sink), route->flip.flips);
  gst_clear_object (&route->sinkpad);
  gst_clear_object (&route->sink);
}
