#define _GNU_SOURCE
#include <gst/gst.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../lgroute.h"

static const char *graph;
static gboolean graph_ok = TRUE, receiver = TRUE, writes_ok = TRUE, short_write;
static int apply_result, applies, formats, gains, mutes, unmutes, graph_reads, player_volume = -1;
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
  g_assert_cmpstr (destination, ==, "/proc/aaudio/instances/04260026/prop");
  static const char expected[] =
      "in-sampling-rate 192000\n"
      "in-channels 8\n"
      "in-bit-per-sample 16\n"
      "endian 0\n"
      "sign 0\n"
      "out-sampling-rate 192000\n"
      "out-channels 8\n"
      "out-bit-per-sample 16\n"
      "gain-enable false\n";
  g_assert_cmpmem (buffer, size, expected, sizeof expected - 1);
  if (short_write) return size - 1;
  formats++; gains++;
  return size;
}

static int close_property (int fd) { g_assert_cmpint (fd, ==, 123); return 0; }
int lgroute_control_receiver (void) { return receiver; }
int lgroute_control_check (void) { return 0; }
int lgroute_control_open (LgRouteControl *control) { (void) control; return 0; }
int lgroute_control_apply (LgRouteControl *control) { (void) control; applies++; return apply_result; }
int lgroute_control_release (LgRouteControl *control) { (void) control; return 0; }
#define g_file_get_contents read_graph
#define open open_property
#define write write_property
#define close close_property
#include "../lgroute.c"
#undef g_file_get_contents
#undef open
#undef write
#undef close

typedef struct { GstElement parent; gboolean pcm; gint volume; } TestSink;
typedef struct { GstElementClass parent; } TestSinkClass;
G_DEFINE_TYPE (TestSink, test_sink, GST_TYPE_ELEMENT)
static void set_property (GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
  TestSink *sink = (TestSink *) object;
  (void) spec;
  if (id == 1) { sink->pcm = g_value_get_boolean (value); return; }
  const GstStructure *fade = gst_value_get_structure (value);
  gint duration = -1;
  g_assert_true (gst_structure_get_int (fade, "target-volume", &sink->volume));
  g_assert_true (gst_structure_get_int (fade, "duration-in-ms", &duration) && duration == 0);
  g_assert_cmpstr (gst_structure_get_string (fade, "fade"), ==, "Linear");
  if (sink->volume == 0) mutes++; else { g_assert_cmpint (sink->volume, ==, 100); unmutes++; }
}
static void get_property (GObject *object, guint id, GValue *value, GParamSpec *spec)
{
  TestSink *sink = (TestSink *) object;
  (void) spec;
  if (id == 1) g_value_set_boolean (value, sink->pcm);
  else g_value_set_int (value, player_volume >= 0 ? player_volume : sink->volume);
}
static void test_sink_class_init (TestSinkClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS (klass);
  object->set_property = set_property; object->get_property = get_property;
  g_object_class_install_property (object, 1, g_param_spec_boolean ("pcm-multi-ch", "pcm", "pcm", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property (object, 2, g_param_spec_boxed ("fade", "fade", "fade", GST_TYPE_STRUCTURE, G_PARAM_WRITABLE));
  g_object_class_install_property (object, 3, g_param_spec_int ("fade-volume", "volume", "volume", 0, 100, 100, G_PARAM_READABLE));
}
static void test_sink_init (TestSink *self) { self->volume = 100; }

static const char before[] =
  "lipsync_04230003_00000000e762b497_out0 -> gain_041b0006_00000000b582ed71_in0;\n";
static const char after[] =
  "rts_0425000a_000000001891f88e_out0 -> srcbin_04260026_00000000ad31c67e_in0;\n"
  "srcbin_04260026_00000000ad31c67e_out0 -> lipsync_04230035_00000000b582ed71_in0;\n"
  "lipsync_04230034_000000005b58d631_out0 -> gain_041b0021_00000000658bf012_in0;\n"
  "lipsync_04230003_00000000e762b497_out0 -> gain_041b0006_00000000b582ed71_in0;\n";

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

static GstPadProbeReturn
carrier (LgRoute *route, gboolean expect_es)
{
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, 32768, NULL);
  GST_BUFFER_FLAG_SET (buffer, ES_FLAG);
  GstPadProbeInfo info = {0}; info.type = GST_PAD_PROBE_TYPE_BUFFER; info.data = buffer;
  GstPadProbeReturn result = buffer_probe (NULL, &info, route);
  if (result == GST_PAD_PROBE_OK)
    g_assert_cmpint (!!GST_BUFFER_FLAG_IS_SET ((GstBuffer *) info.data, ES_FLAG), ==, expect_es);
  gst_buffer_unref (info.data);
  return result;
}

static void reset (LgRoute *route)
{
  lgroute_init (route, NULL);
  route->sink = g_object_new (test_sink_get_type (), NULL);
  graph = before; graph_ok = receiver = writes_ok = TRUE; apply_result = graph_reads = 0;
  short_write = FALSE;
  player_volume = -1;
}

int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  LgRoute route;
  reset (&route);
  g_assert_false (verified_sink (route.sink));
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); g_assert_cmpint (formats, ==, 0);
  lgroute_disarm (&route); g_assert_cmpint (mutes + unmutes, ==, 0);

  reset (&route);
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (mutes, ==, 1);
  graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (route.configured); g_assert_false (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (graph_reads, ==, 2);
  g_assert_cmpint (formats, ==, 1); g_assert_cmpint (gains, ==, 1);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (applies, ==, 1); g_assert_cmpint (mutes, ==, 1);
  player_volume = 100; route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (mutes, ==, 2);
  player_volume = -1;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  lgroute_disarm (&route); g_assert_cmpint (unmutes, ==, 1);

  reset (&route); caps_event (&route); graph = before;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  g_assert_cmpint (unmutes, ==, 2);

  reset (&route); caps_event (&route);
  gchar *ambiguous = g_strconcat (after,
      "rts_0425000b_000000001891f88e_out0 -> srcbin_04260027_00000000ad31c67e_in0;\n"
      "srcbin_04260027_00000000ad31c67e_out0 -> lipsync_04230036_00000000b582ed71_in0;\n", NULL);
  graph = ambiguous;
  int old_formats = formats;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_cmpint (formats, ==, old_formats);
  lgroute_disarm (&route); g_free (ambiguous);

  reset (&route); caps_event (&route); graph = after; writes_ok = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; short_write = TRUE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; apply_result = -1;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; receiver = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); graph_ok = FALSE;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  g_print ("LG route: converter ownership, fade isolation, carrier flags, CAPS changes and failure refusal pass\n");
  return 0;
}
