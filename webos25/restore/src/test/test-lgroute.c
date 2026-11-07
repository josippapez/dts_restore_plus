#define _GNU_SOURCE
#include <gst/gst.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../lgroute.h"
#include "../lgroute-graph.h"

static const char *graph;
static gboolean graph_ok = TRUE, receiver = TRUE, writes_ok = TRUE, short_write;
static int unmixed_result = 1, checks, formats, graph_reads, pcm_reports, restores;
static char destination[128];

static gboolean
read_graph (const gchar *filename, gchar **contents, gsize *length, GError **error)
{
  (void) error;
  g_assert_cmpstr (filename, ==, "/proc/aaudio/dot_module_connection");
  graph_reads++;
  if (!graph_ok) return FALSE;
  *contents = g_strdup (graph);
  if (length) *length = strlen (graph);
  return TRUE;
}

static int
open_property (const char *filename, int flags, ...)
{
  g_assert_true ((flags & O_ACCMODE) == O_WRONLY);
  g_strlcpy (destination, filename, sizeof destination);
  return 123;
}

static ssize_t
write_property (int fd, const void *buffer, size_t size)
{
  g_assert_cmpint (fd, ==, 123);
  if (!writes_ok) return -1;
  g_assert_cmpstr (destination, ==, "/proc/aaudio/instances/04260026/cmd");
  static const char expected[] = "0x50020020 192000 8 16 0 0 192000 8 16\n";
  g_assert_cmpmem (buffer, size, expected, sizeof expected - 1);
  if (short_write) return size - 1;
  formats++;
  return size;
}

static int close_property (int fd) { g_assert_cmpint (fd, ==, 123); return 0; }
int lgroute_control_receiver (void) { return receiver; }
int lgroute_control_check (void) { return 0; }
int lgroute_control_unmixed (void) { checks++; return unmixed_result; }
int lgroute_input_pipeline (char *pipeline, size_t size) { g_strlcpy (pipeline, "_p1", size); return 0; }
int lgroute_input_query (const char *pipeline, char *codec, size_t codec_size,
    char *immersive, size_t immersive_size)
{
  g_assert_cmpstr (pipeline, ==, "_p1");
  g_strlcpy (codec, "dts", codec_size); g_strlcpy (immersive, "none", immersive_size);
  return 0;
}
int lgroute_input_set (const char *pipeline, const char *resource,
    const char *codec, const char *channel, const char *immersive)
{
  g_assert_cmpstr (pipeline, ==, "_p1"); g_assert_cmpstr (resource, ==, "ADEC1");
  g_assert_cmpstr (immersive, ==, "none");
  if (!g_strcmp0 (codec, "pcm")) { g_assert_cmpstr (channel, ==, "8"); pcm_reports++; }
  else { g_assert_cmpstr (codec, ==, "dts"); g_assert_null (channel); restores++; }
  return 0;
}
#define g_file_get_contents read_graph
#define open open_property
#define write write_property
#define close close_property
#include "../lgroute.c"
#undef g_file_get_contents
#undef open
#undef write
#undef close

typedef struct { GstElement parent; gboolean pcm; } TestSink;
typedef struct { GstElementClass parent; } TestSinkClass;
G_DEFINE_TYPE (TestSink, test_sink, GST_TYPE_ELEMENT)
static void set_property (GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
  (void) id; (void) spec;
  ((TestSink *) object)->pcm = g_value_get_boolean (value);
}
static void get_property (GObject *object, guint id, GValue *value, GParamSpec *spec)
{
  (void) id; (void) spec;
  g_value_set_boolean (value, ((TestSink *) object)->pcm);
}
static void test_sink_class_init (TestSinkClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS (klass);
  object->set_property = set_property; object->get_property = get_property;
  g_object_class_install_property (object, 1, g_param_spec_boolean ("pcm-multi-ch", "pcm", "pcm", FALSE, G_PARAM_READWRITE));
}
static void test_sink_init (TestSink *self) { (void) self; }

static const char before[] =
  "digraph {\n}\n";
static const char after[] =
  "digraph {\n"
  "gain_041b0021_0000000000000001_out0 -> mixerbin_04270001_0000000000000002_in0;\n"
  "appsrc_08110001_0000000000000001_out0 -> srcbin_04260025_0000000000000002_in0;\n"
  "srcbin_04260025_0000000000000001_out0 -> lipsync_04230034_0000000000000002_in0;\n"
  "appsrc_08110002_0000000000000001_out0 -> rts_0425000a_0000000000000002_in0;\n"
  "rts_0425000a_000000001891f88e_out0 -> srcbin_04260026_00000000ad31c67e_in0;\n"
  "srcbin_04260026_00000000ad31c67e_out0 -> lipsync_04230035_00000000b582ed71_in0;\n"
  "lipsync_04230034_000000005b58d631_out0 -> gain_041b0021_00000000658bf012_in0;\n"
  "lipsync_04230003_00000000e762b497_out0 -> gain_041b0006_00000000b582ed71_in0;\n}\n";

static GstPadProbeReturn
caps_event (LgRoute *route)
{
  GstCaps *caps = lgroute_carrier_caps ();
  GstEvent *event = gst_event_new_caps (caps);
  gst_caps_unref (caps);
  GstPadProbeInfo info = {0};
  info.type = GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM; info.data = event;
  GstPadProbeReturn result = event_probe (NULL, &info, route);
  gst_event_unref (event);
  return result;
}

static gboolean last_silent;

static GstPadProbeReturn
carrier (LgRoute *route, gboolean expect_es)
{
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, 32768, NULL);
  gst_buffer_memset (buffer, 0, 0x5a, 32768);
  GST_BUFFER_FLAG_SET (buffer, ES_FLAG);
  GstPadProbeInfo info = {0}; info.type = GST_PAD_PROBE_TYPE_BUFFER; info.data = buffer;
  GstPadProbeReturn result = buffer_probe (NULL, &info, route);
  if (result == GST_PAD_PROBE_OK) {
    guint8 byte = 1;
    gst_buffer_extract (info.data, 100, &byte, 1);
    last_silent = byte == 0;
    g_assert_cmpint (!!GST_BUFFER_FLAG_IS_SET ((GstBuffer *) info.data, ES_FLAG), ==, expect_es);
  }
  gst_buffer_unref (info.data);
  return result;
}

static void reset (LgRoute *route)
{
  lgroute_init (route, NULL);
  route->sink = g_object_new (test_sink_get_type (), NULL);
  g_strlcpy (route->resource, "ADEC1", sizeof route->resource);
  graph = before; graph_ok = receiver = writes_ok = TRUE; graph_reads = 0; unmixed_result = 1;
  short_write = FALSE;
}

static gchar *replace_text (const gchar *text, const gchar *old, const gchar *replacement)
{
  gchar **parts = g_strsplit (text, old, -1);
  gchar *result = g_strjoinv (replacement, parts);
  g_strfreev (parts);
  return result;
}

static void handoff_tests (void)
{
  for (unsigned sequence = 0; sequence < 3; sequence++) {
    LgRoute route;
    reset (&route);
    TestSink *sink = (TestSink *) g_object_ref (route.sink);
    g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
    graph = after;
    g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
    GstPad *pad = gst_pad_new ("handoff", GST_PAD_SINK);
    gst_pad_set_active (pad, TRUE);
    route.sinkpad = gst_object_ref (pad);
    route.events = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        event_probe, &route, NULL);
    route.buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER,
        buffer_probe, &route, NULL);
    gst_pad_send_event (pad, gst_event_new_stream_start ("replacement"));
    GstCaps *caps = gst_caps_from_string (sequence == 0 ? "audio/x-ac3" :
        sequence == 1 ? "audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved" :
        "audio/x-eac3");
    gst_pad_send_event (pad, gst_event_new_caps (caps));
    gst_caps_unref (caps);
    GstEvent *sticky = gst_pad_get_sticky_event (pad, GST_EVENT_CAPS, 0);
    g_assert_nonnull (sticky);
    gst_event_unref (sticky);
    g_assert_null (route.sink);
    g_assert_cmpuint (route.events + route.buffers, ==, 0);
    g_assert_false (sink->pcm);
    lgroute_disarm (&route);
    gst_pad_set_active (pad, FALSE);
    gst_object_unref (pad);
    gst_object_unref (sink);
  }
  LgRoute route;
  reset (&route);
  TestSink *sink = (TestSink *) g_object_ref (route.sink);
  caps_event (&route);
  g_assert_true (sink->pcm);
  lgroute_disarm (&route);
  g_assert_false (sink->pcm);
  gst_object_unref (sink);
}

static void graph_tests (void)
{
  gchar uid[9] = "";
  g_assert_true (lgroute_bypass_converter (after, uid));
  g_assert_cmpstr (uid, ==, "04260026");
  g_assert_false (lgroute_bypass_converter (before, uid));
  /* the main head is srcbin -> lipsync without an rts; it is never chosen */
  gchar *changed = replace_text (after, "rts_0425000a_000000001891f88e_out0 ->", "queue_0418000a_000000001891f88e_out0 ->");
  g_assert_false (lgroute_bypass_converter (changed, uid));
  g_free (changed);
  changed = replace_text (after, "}\n",
      "rts_042500ff_0000000000000001_out0 -> srcbin_042600ff_0000000000000002_in0;\n"
      "srcbin_042600ff_0000000000000001_out0 -> lipsync_042300ff_0000000000000002_in0;\n}\n");
  g_assert_false (lgroute_bypass_converter (changed, uid));
  g_free (changed);
  changed = replace_text (after, "rts_0425000a_000000001891f88e_out0 ->", "gluepw_08130001_out ->");
  g_assert_false (lgroute_bypass_converter (changed, uid));
  g_free (changed);
}

int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  graph_tests ();
  LgRoute route;
  reset (&route);
  g_assert_false (verified_sink (route.sink));
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); g_assert_cmpint (formats, ==, 0);
  lgroute_disarm (&route);

  reset (&route);
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (route.configured); g_assert_false (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (graph_reads, ==, 1);
  g_assert_cmpint (formats, ==, 1);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (checks, ==, 1);
  route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (checks, ==, 2);
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = before;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route);
    gchar *ambiguous = replace_text (after, "}\n",
      "rts_0425000a_000000001891f88e_out0 -> srcbin_04260027_00000000ad31c67e_in0;\n"
      "srcbin_04260027_00000000ad31c67e_out0 -> lipsync_04230036_00000000b582ed71_in0;\n}\n");
  graph = ambiguous;
  int old_formats = formats;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_cmpint (formats, ==, old_formats);
  lgroute_disarm (&route); g_free (ambiguous);

  reset (&route); caps_event (&route); graph = after; writes_ok = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  GstEvent *stream = gst_event_new_stream_start ("replacement");
  GstPadProbeInfo stream_info = {0};
  stream_info.type = GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM;
  stream_info.data = stream;
  g_assert_cmpint (event_probe (NULL, &stream_info, &route), ==, GST_PAD_PROBE_OK);
  gst_event_unref (stream);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; short_write = TRUE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; unmixed_result = -1;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  /* A brief LG policy rewrite is tolerated; a lasting mix falls back. */
  reset (&route); caps_event (&route); graph = after; unmixed_result = 0;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (route.mixed_since, !=, 0);
  unmixed_result = 1; route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (route.mixed_since, ==, 0);
  unmixed_result = 0; route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  route.mixed_since -= G_TIME_SPAN_SECOND; route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); g_assert_cmpstr (route.reason, ==, "LG keeps eARC mixing on");
  lgroute_disarm (&route);

  /* Carrier is silence until LG unmixes; a mix re-reports PCM; disarm restores LG's codec. */
  reset (&route); pcm_reports = restores = 0;
  caps_event (&route); graph = after;
  g_assert_cmpint (pcm_reports, ==, 0);
  unmixed_result = 0;
  /* Reported once the bypass exists, and again because LG is still mixing. */
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); g_assert_cmpint (pcm_reports, ==, 2);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); g_assert_cmpint (pcm_reports, ==, 2);
  unmixed_result = 1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  lgroute_disarm (&route);
  g_assert_cmpint (restores, ==, 1);
  g_assert_cmpstr (route.pipeline, ==, "");

  reset (&route); caps_event (&route); graph = after; receiver = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); graph_ok = FALSE;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  handoff_tests ();
  g_print ("LG route: bypass converter, carrier flags, CAPS changes and failure refusal pass\n");
  return 0;
}
