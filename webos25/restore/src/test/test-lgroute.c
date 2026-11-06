#define _GNU_SOURCE
#include <gst/gst.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../lgroute.h"

static const char *graph;
static gboolean graph_ok = TRUE, receiver = TRUE, writes_ok = TRUE;
static int apply_result, applies, formats, gains, mutes, unmutes, graph_reads;
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
  if (g_str_has_suffix (destination, "/cmd")) {
    g_assert_cmpstr (destination, ==, "/proc/aaudio/instances/04260026/cmd");
    if (!strncmp (buffer, "0x50020020 ", 11)) formats++;
    else { g_assert_true (!strncmp (buffer, "0x50040008 ", 11)); gains++; }
  } else {
    g_assert_cmpstr (destination, ==, "/proc/aaudio/instances/041b0021/prop");
    if (!strncmp (buffer, "mute-input 1", 12)) mutes++;
    else { g_assert_true (!strncmp (buffer, "mute-input 0", 12)); unmutes++; }
  }
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

typedef struct { GstElement parent; gboolean pcm; } TestSink;
typedef struct { GstElementClass parent; } TestSinkClass;
G_DEFINE_TYPE (TestSink, test_sink, GST_TYPE_ELEMENT)
static void set_property (GObject *object, guint id, const GValue *value, GParamSpec *spec)
{ (void) id; (void) spec; ((TestSink *) object)->pcm = g_value_get_boolean (value); }
static void get_property (GObject *object, guint id, GValue *value, GParamSpec *spec)
{ (void) id; (void) spec; g_value_set_boolean (value, ((TestSink *) object)->pcm); }
static void test_sink_class_init (TestSinkClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS (klass);
  object->set_property = set_property; object->get_property = get_property;
  g_object_class_install_property (object, 1, g_param_spec_boolean ("pcm-multi-ch", "pcm", "pcm", FALSE, G_PARAM_READWRITE));
}
static void test_sink_init (TestSink *self) { (void) self; }

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
}

int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
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
  g_assert_cmpint (graph_reads, ==, 2);
  g_assert_cmpint (formats, ==, 1); g_assert_cmpint (gains, ==, 1);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (applies, ==, 1);
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  lgroute_disarm (&route); g_assert_cmpint (unmutes, ==, 1);

  reset (&route); caps_event (&route); graph = before;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route);
  gchar *ambiguous = g_strconcat (after,
      "lipsync_04230038_000000005b58d631_out0 -> gain_041b0022_00000000658bf012_in0;\n", NULL);
  graph = ambiguous;
  int old_mutes = mutes, old_unmutes = unmutes;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_cmpint (mutes, ==, old_mutes); g_assert_cmpstr (route.main_gain, ==, "");
  lgroute_disarm (&route); g_assert_cmpint (unmutes, ==, old_unmutes); g_free (ambiguous);

  reset (&route); caps_event (&route);
  ambiguous = g_strconcat (after,
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

  reset (&route); caps_event (&route); graph = after; apply_result = -1;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); caps_event (&route); graph = after; receiver = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); graph_ok = FALSE;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  g_print ("LG route: graph ownership, ambiguous/main isolation, carrier flags, CAPS changes and failure refusal pass\n");
  return 0;
}
