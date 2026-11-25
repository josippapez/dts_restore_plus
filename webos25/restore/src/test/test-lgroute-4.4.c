/* Host test for the kernel 4.4 (webOS 6) route: lgroute-4.4.c's sink wait and log line.
 * Run it against Debian buster's GStreamer 1.14 / GLib 2.58 to match the C1's GLib.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <string.h>
#define LGROUTE_RUN "/tmp/lgroute-4.4-test-run"
#define LGROUTE_SINK_WAIT (600 * G_TIME_SPAN_MILLISECOND)
#define LGROUTE_LEASE LGROUTE_RUN "/earc_route"
#include "../lgroute-4.4.c"

int lgroute_control_receiver (void) { return 1; }
int lgroute_control_unmix (void) { return 0; }
int lgroute_control_unmixed_why (char why[LGROUTE_WHY]) { (void) why; return 1; }

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
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass), "test sink", "Sink/Audio", "stands in for LG's sink", "test");
}
static void test_sink_init (TestSink *self) { (void) self; }

static gpointer add_sink_late (gpointer pipeline)
{
  g_usleep (150 * G_TIME_SPAN_MILLISECOND);
  gst_bin_add (GST_BIN (pipeline), gst_element_factory_make ("audiosink", NULL));
  return NULL;
}

static gchar *route_log (void)
{
  gchar *text = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
  return text;
}

int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_mkdir_with_parents (LGROUTE_RUN, 0700);
  g_unlink (ROUTE_LOG);

  /* A C1 tester's log printed "(null)" for every time: %f is GLib 2.66. */
  lgroute_log ("hello");
  gchar *text = route_log ();
  g_assert_true (g_regex_match_simple ("^[0-2][0-9]:[0-5][0-9]:[0-6][0-9]\\.[0-9]{3} hello\n$", text, 0, 0));
  g_free (text);

  /* No LG sink in a pipeline that is not starting: refused at once, with what it holds. */
  {
    GstElement *pipeline = gst_pipeline_new ("player"), *decoder = gst_bin_new ("decoder");
    GstElement *other = gst_element_factory_make ("fakesink", NULL);
    g_assert_nonnull (other);
    gst_bin_add_many (GST_BIN (pipeline), decoder, other, NULL);
    LgRoute route;
    lgroute_init (&route, decoder);
    g_unlink (ROUTE_LOG);
    gint64 start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&route));
    g_assert_cmpint (g_get_monotonic_time () - start, <, 100 * G_TIME_SPAN_MILLISECOND);
    text = route_log ();
    g_assert_nonnull (strstr (text, "not armed: 0 verified LG sinks in the pipeline "
        "(0 audiosink elements; player is NULL, going VOID_PENDING; sinks: fakesink)\n"));
    g_free (text);
    gst_object_unref (pipeline);
  }

  /* The C1's first DTS frame can come before LG's audio chain exists: a starting
   * pipeline waits for its audiosink instead of sending the core only. */
  {
    GstElement *pipeline = gst_pipeline_new ("player"), *decoder = gst_bin_new ("decoder");
    gst_bin_add (GST_BIN (pipeline), decoder);
    g_assert_true (gst_element_register (NULL, "audiosink", GST_RANK_NONE, test_sink_get_type ()));
    LgRoute route;
    lgroute_init (&route, decoder);
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
    GThread *late = g_thread_new ("late-sink", add_sink_late, pipeline);
    g_unlink (ROUTE_LOG);
    gint64 start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&route));   /* found, but not LG's verified build */
    gint64 waited = g_get_monotonic_time () - start;
    g_thread_join (late);
    g_assert_cmpint (waited, >=, 100 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint (waited, <, 500 * G_TIME_SPAN_MILLISECOND);
    text = route_log ();
    g_assert_nonnull (strstr (text, "not armed: 0 verified LG sinks in the pipeline (1 audiosink elements; player is"));
    g_free (text);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
  }

  g_unlink (ROUTE_LOG);
  g_rmdir (LGROUTE_RUN);
  g_print ("ok   lgroute-4.4: log times, sink wait, refusal detail\n");
  return 0;
}
