#define _GNU_SOURCE
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define LGROUTE_RUN "/tmp/lgroute-test-run"
#define LGROUTE_INSTANCES "/tmp/lgroute-test-run/instances"
#include "../lgroute.h"
#include "../lgroute-graph.h"

static const char *graph;
static gboolean graph_ok = TRUE, receiver = TRUE, writes_ok = TRUE, short_write;
static int unmixed_result = 1, checks, formats, graph_reads;
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
  g_assert_cmpstr (destination, ==, LGROUTE_INSTANCES "/04260026/cmd");
  static const char expected[] = "0x50020020 192000 8 16 0 0 192000 8 16\n";
  g_assert_cmpmem (buffer, size, expected, sizeof expected - 1);
  if (short_write) return size - 1;
  formats++;
  return size;
}

static int close_property (int fd) { g_assert_cmpint (fd, ==, 123); return 0; }
int lgroute_control_receiver (void) { return receiver; }
int lgroute_control_unmixed (void) { checks++; return unmixed_result; }
static gchar luna_dir[] = "/tmp/lgroute-luna-XXXXXX", luna_send[64];
#define LUNA_SEND luna_send

/* Every luna-send call, with the thread that made it. */
static GMutex calls_lock;
static GPtrArray *calls;
static GThread *call_thread[64];
static int call_count;

static gboolean
record_spawn (const gchar *dir, gchar **argv, gchar **envp, GSpawnFlags flags,
    GSpawnChildSetupFunc setup, gpointer data, gchar **out, gchar **err, gint *status, GError **error)
{
  g_mutex_lock (&calls_lock);
  if (!calls) calls = g_ptr_array_new_with_free_func (g_free);
  g_assert_cmpint (call_count, <, 64);
  call_thread[call_count++] = g_thread_self ();
  g_ptr_array_add (calls, g_strdup_printf ("%s %s", argv[5], argv[6]));
  g_mutex_unlock (&calls_lock);
  return g_spawn_sync (dir, argv, envp, flags, setup, data, out, err, status, error);
}
#define g_spawn_sync record_spawn

/* Count LG input reports from the fake luna-send's log. */
static int luna_count (const gchar *needle)
{
  gchar *path = g_build_filename (luna_dir, "log", NULL), *log = NULL;
  int n = 0;
  if (g_file_get_contents (path, &log, NULL, NULL))
    for (const gchar *p = log; (p = strstr (p, needle)); p++) n++;
  g_free (path); g_free (log);
  return n;
}

static gboolean
read_or_graph (const gchar *filename, gchar **contents, gsize *length, GError **error)
{
  if (!strcmp (filename, "/proc/self/cmdline")) {
    static const char args[] = "starfish-media-pipeline\0pipeline_process\0-scom.webos.pipeline._p1\0";
    *contents = g_memdup2 (args, sizeof args - 1);
    *length = sizeof args - 1;
    return TRUE;
  }
  if (strcmp (filename, "/proc/aaudio/dot_module_connection"))
    return g_file_get_contents (filename, contents, length, error);
  return read_graph (filename, contents, length, error);
}
#define g_file_get_contents read_or_graph
#define open open_property
#define write write_property
#define close close_property
#include "../lgroute.c"
#undef g_file_get_contents
#undef open
#undef write
#undef close

/* Wait for the luna worker to finish everything queued. */
static void drain (void)
{
  for (int i = 0; i < 2000 && g_atomic_int_get (&outstanding); i++) g_usleep (10000);
  g_assert_cmpint (g_atomic_int_get (&outstanding), ==, 0);
}

static int calls_matching (const gchar *needle)
{
  int n = 0;
  g_mutex_lock (&calls_lock);
  for (guint i = 0; calls && i < calls->len; i++)
    if (strstr (g_ptr_array_index (calls, i), needle)) n++;
  g_mutex_unlock (&calls_lock);
  return n;
}

static void forget_calls (void)
{
  drain ();
  g_mutex_lock (&calls_lock);
  if (calls) g_ptr_array_set_size (calls, 0);
  call_count = 0;
  g_mutex_unlock (&calls_lock);
}

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

static void set_lg_codec (const gchar *codec)
{
  gchar *path = g_build_filename (luna_dir, "state", NULL);
  g_file_set_contents (path, codec, -1, NULL);
  g_free (path);
}

static void reset (LgRoute *route)
{
  drain ();
  set_lg_codec ("dts");
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

static GstPadProbeReturn
send_event (LgRoute *route, GstEvent *event)
{
  GstPadProbeInfo info = {0};
  info.type = GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM; info.data = event;
  GstPadProbeReturn result = event_probe (NULL, &info, route);
  gst_event_unref (event);
  return result;
}

static gboolean accept_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  (void) pad; (void) parent;
  gst_event_unref (event);
  return TRUE;
}

static GstFlowReturn accept_buffer (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  (void) pad; (void) parent;
  gst_buffer_unref (buffer);
  return GST_FLOW_OK;
}

/* Resume and seek: a pad probe built as lgroute_arm builds it must see flushes. */
static void resume_tests (void)
{
  LgRoute route;

  /* 1. lgroute_arm's probe mask delivers FLUSH_STOP to the handler. */
  reset (&route);
  GstPad *pad = gst_pad_new ("flush", GST_PAD_SINK), *source = gst_pad_new ("src", GST_PAD_SRC);
  g_assert_true (gst_pad_set_active (pad, TRUE));
  g_assert_true (gst_pad_set_active (source, TRUE));
  g_assert_cmpint (gst_pad_link (source, pad), ==, GST_PAD_LINK_OK);
  route.sinkpad = gst_object_ref (pad);
  route.events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, &route, NULL);
  route.next_check = g_get_monotonic_time () + 60 * G_TIME_SPAN_SECOND;
  route.unmixed = TRUE;
  gst_pad_push_event (source, gst_event_new_flush_start ());
  gst_pad_push_event (source, gst_event_new_flush_stop (TRUE));
  g_assert_cmpint (route.next_check, ==, 0);
  g_assert_false (route.unmixed);
  lgroute_disarm (&route);
  gst_pad_unlink (source, pad);
  gst_object_unref (source); gst_object_unref (pad);

  /* 2. Configured route: a repeated STREAM_START with the same id keeps it. */
  reset (&route);
  send_event (&route, gst_event_new_stream_start ("a"));
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  send_event (&route, gst_event_new_stream_start ("a"));
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_false (route.failed); g_assert_true (route.configured);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);

  /* 3. A new id: the renderer restarts, so setup is redone with the new caps. */
  int formats_before = formats;
  send_event (&route, gst_event_new_stream_start ("b"));
  g_assert_false (route.failed); g_assert_false (route.configured);
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);   /* first ES event flagged again */
  g_assert_true (route.configured); g_assert_cmpint (formats, ==, formats_before + 1);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  lgroute_disarm (&route);

  /* 4. A new id whose caps never arrive fails cleanly instead of passing carrier. */
  reset (&route);
  send_event (&route, gst_event_new_stream_start ("a"));
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  send_event (&route, gst_event_new_stream_start ("b"));
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  lgroute_disarm (&route);

  /* 5. LG switches eARC back to mixed between two buffers well inside the old
   * 250 ms cache: the second buffer must be silence, not carrier. */
  reset (&route);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  route.next_check = g_get_monotonic_time () + 60 * G_TIME_SPAN_SECOND;
  unmixed_result = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  unmixed_result = 1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  lgroute_disarm (&route);

  /* 6. After a flush the first carrier buffer re-reads the mixer: mixed means silence. */
  reset (&route);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  route.configured_at -= 2 * G_TIME_SPAN_SECOND;   /* a seek well into playback */
  send_event (&route, gst_event_new_flush_stop (TRUE));
  g_assert_false (route.failed);
  unmixed_result = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  lgroute_disarm (&route);

  /* 7. A track switch on a real pad: the new id comes without caps, because the
   * carrier caps stay sticky. LG's sink restarts its renderer, so setup is redone. */
  reset (&route);
  pad = gst_pad_new ("switch", GST_PAD_SINK); source = gst_pad_new ("src", GST_PAD_SRC);
  gst_pad_set_event_function (pad, accept_event);
  gst_pad_set_chain_function (pad, accept_buffer);
  g_assert_true (gst_pad_set_active (pad, TRUE));
  g_assert_true (gst_pad_set_active (source, TRUE));
  g_assert_cmpint (gst_pad_link (source, pad), ==, GST_PAD_LINK_OK);
  route.sinkpad = gst_object_ref (pad);
  route.events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, &route, NULL);
  route.buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, &route, NULL);
  GstCaps *carrier_caps = lgroute_carrier_caps ();
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (source, gst_event_new_stream_start ("track1"));
  gst_pad_push_event (source, gst_event_new_caps (carrier_caps));
  gst_pad_push_event (source, gst_event_new_segment (&segment));
  graph = after;
  formats_before = formats;
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 32768, NULL));
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 32768, NULL));
  g_assert_true (route.configured); g_assert_cmpint (formats, ==, formats_before + 1);
  gst_pad_push_event (source, gst_event_new_stream_start ("track2"));
  g_assert_true (((TestSink *) route.sink)->pcm);   /* set before LG's sink reopens */
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 32768, NULL));
  g_assert_false (route.failed); g_assert_true (route.configured);
  g_assert_cmpint (formats, ==, formats_before + 2);
  g_assert_false (((TestSink *) route.sink)->pcm);  /* the new FIRST leaves as ES */
  gst_caps_unref (carrier_caps);
  lgroute_disarm (&route);
  gst_pad_unlink (source, pad);
  gst_object_unref (source); gst_object_unref (pad);

  /* 8. LG playing the carrier at a quarter of its rate (a CS after resume) fails
   * over to decoding after three slow windows. */
  reset (&route);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  GstPadProbeReturn last = GST_PAD_PROBE_OK;
  for (int window = 0; window < 3; window++) {
    g_assert_false (route.failed);
    route.window_start -= 2 * G_TIME_SPAN_SECOND;
    for (int i = 0; i < 24; i++) last = carrier (&route, FALSE);
  }
  g_assert_cmpint (last, ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpstr (route.reason, ==, "LG plays the carrier too slowly");
  gchar *log = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &log, NULL, NULL));
  g_assert_nonnull (strstr (log, "failed, decoding instead: LG plays the carrier too slowly"));
  g_free (log);
  gst_element_set_state (route.sink, GST_STATE_NULL);
  lgroute_disarm (&route);

  /* 9. Normal pace, two slow windows, and a pause gap never fail the route. */
  reset (&route);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  caps_event (&route); graph = after;
  carrier (&route, TRUE);
  for (int i = 0; i < 24 * 3; i++) g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  for (int window = 0; window < 4; window++) {
    route.window_start -= 2 * G_TIME_SPAN_SECOND;
    if (window == 2) route.last_push -= G_TIME_SPAN_SECOND;   /* a pause between windows */
    for (int i = 0; i < 24; i++) g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  }
  g_assert_false (route.failed);
  /* Not playing (paused, prerolling): never measured. */
  gst_element_set_state (route.sink, GST_STATE_PAUSED);
  for (int window = 0; window < 4; window++) {
    route.window_start -= 2 * G_TIME_SPAN_SECOND;
    for (int i = 0; i < 24; i++) g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  }
  g_assert_false (route.failed);
  gst_element_set_state (route.sink, GST_STATE_NULL);
  lgroute_disarm (&route);

  /* 10. A resume seeks right after setup (every logged resume: 17-26 ms): the
   * renderer restart waits for playback, and a restart that cannot happen decodes. */
  reset (&route);
  forget_calls ();
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  send_event (&route, gst_event_new_flush_stop (TRUE));
  g_assert_false (route.failed); g_assert_true (route.restart_pending);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);   /* no pad to restart on */
  g_assert_true (route.failed);
  g_assert_cmpstr (route.reason, ==, "LG's renderer did not restart");
  gst_element_set_state (route.sink, GST_STATE_NULL);
  lgroute_disarm (&route);

}

/* A route on a real linked pad, so probes see events sent to the pad. */
static void wire (LgRoute *route, GstPad **pad, GstPad **source)
{
  *pad = gst_pad_new ("restart", GST_PAD_SINK); *source = gst_pad_new ("src", GST_PAD_SRC);
  gst_pad_set_event_function (*pad, accept_event);
  gst_pad_set_chain_function (*pad, accept_buffer);
  g_assert_true (gst_pad_set_active (*pad, TRUE));
  g_assert_true (gst_pad_set_active (*source, TRUE));
  g_assert_cmpint (gst_pad_link (*source, *pad), ==, GST_PAD_LINK_OK);
  route->sinkpad = gst_object_ref (*pad);
  route->events = gst_pad_add_probe (*pad, EVENT_PROBE_MASK, event_probe, route, NULL);
  route->buffers = gst_pad_add_probe (*pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL);
  GstCaps *caps = lgroute_carrier_caps ();
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (*source, gst_event_new_stream_start ("movie"));
  gst_pad_push_event (*source, gst_event_new_caps (caps));
  gst_pad_push_event (*source, gst_event_new_segment (&segment));
  gst_caps_unref (caps);
}

static void unwire (LgRoute *route, GstPad *pad, GstPad *source)
{
  gst_element_set_state (route->sink, GST_STATE_NULL);
  lgroute_disarm (route);
  gst_pad_unlink (source, pad);
  gst_object_unref (source); gst_object_unref (pad);
}

/* The pushed buffer's first byte as LG's sink would get it (0 = silence). */
static guint8 pushed_byte;
static GstPadProbeReturn after_route (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  (void) pad; (void) data;
  gst_buffer_extract (GST_PAD_PROBE_INFO_BUFFER (info), 0, &pushed_byte, 1);
  return GST_PAD_PROBE_OK;
}

static void push_carrier (GstPad *source)
{
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, 32768, NULL);
  gst_buffer_memset (buffer, 0, 0x5a, 32768);
  GST_BUFFER_FLAG_SET (buffer, ES_FLAG);
  gst_pad_push (source, buffer);
}

static gchar *pad_stream_id (GstPad *pad)
{
  GstEvent *event = gst_pad_get_sticky_event (pad, GST_EVENT_STREAM_START, 0);
  const gchar *id = NULL;
  gst_event_parse_stream_start (event, &id);
  gchar *copy = g_strdup (id);
  gst_event_unref (event);
  return copy;
}

/* A resume or a rebuild restarts LG's renderer instead of decoding. */
static void restart_tests (void)
{
  LgRoute route;
  GstPad *pad, *source;

  /* 12. Resume: silence while paused, then a new stream id makes LG restart its
   * renderer and the setup is redone with a fresh start event. */
  reset (&route); forget_calls ();
  graph = after;
  wire (&route, &pad, &source);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, after_route, NULL, NULL);
  int formats_before = formats;
  push_carrier (source);
  push_carrier (source);
  g_assert_true (route.configured); g_assert_cmpint (formats, ==, formats_before + 1);
  gst_pad_push_event (source, gst_event_new_flush_start ());
  gst_pad_push_event (source, gst_event_new_flush_stop (TRUE));
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (source, gst_event_new_segment (&segment));
  g_assert_false (route.failed); g_assert_true (route.restart_pending);
  gst_element_set_state (route.sink, GST_STATE_PAUSED);
  push_carrier (source);
  g_assert_cmpint (pushed_byte, ==, 0);                 /* quiet until it plays */
  g_assert_true (route.restart_pending);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  push_carrier (source);
  g_assert_false (route.failed); g_assert_false (route.restart_pending);
  g_assert_true (route.configured); g_assert_cmpint (formats, ==, formats_before + 2);
  g_assert_false (((TestSink *) route.sink)->pcm);    /* the new start leaves as ES */
  gchar *id = pad_stream_id (pad);
  g_assert_cmpstr (id, ==, "movie/lgroute-restart1");
  g_free (id);
  unwire (&route, pad, source);

  /* 13. A rebuild mid-play restarts too, and only twice: the third gives up. */
  reset (&route); forget_calls ();
  graph = after;
  wire (&route, &pad, &source);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  push_carrier (source);
  for (int rebuild = 1; rebuild <= 3; rebuild++) {
    g_assert_cmpint (g_rmdir (LGROUTE_INSTANCES "/04260026"), ==, 0);
    push_carrier (source);
    g_assert_cmpint (g_mkdir (LGROUTE_INSTANCES "/04260026", 0700), ==, 0);
    if (rebuild < 3) {
      g_assert_false (route.failed); g_assert_true (route.configured);
      g_assert_cmpuint (route.restarts, ==, rebuild);
    } else {
      g_assert_true (route.failed);
      g_assert_cmpstr (route.reason, ==, "LG rebuilt its audio path without our setup");
    }
  }
  unwire (&route, pad, source);
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

typedef struct { LgRoute *route; gint *go; } Disarmer;

static gpointer disarm_thread (gpointer data)
{
  Disarmer *d = data;
  while (!g_atomic_int_get (d->go)) g_thread_yield ();
  lgroute_disarm (d->route);
  return NULL;
}

static void async_tests (void)
{
  LgRoute route;
  /* Luna calls leave the calling thread, in order: query, report, then the restore. */
  reset (&route);
  forget_calls ();
  report_pcm (&route);
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (call_count, ==, 4);
  for (int i = 0; i < call_count; i++) g_assert_true (call_thread[i] != g_thread_self ());
  g_assert_nonnull (strstr (g_ptr_array_index (calls, 0), "getAudioInputProperty"));
  g_assert_nonnull (strstr (g_ptr_array_index (calls, 1), "\"codec\":\"pcm\",\"channel\":\"8\""));
  g_assert_nonnull (strstr (g_ptr_array_index (calls, 2), "getAudioInputProperty"));
  g_assert_nonnull (strstr (g_ptr_array_index (calls, 3), "\"codec\":\"dts\",\"immersive\":\"none\""));

  /* LG already described the next stream: the late restore must not overwrite it. */
  reset (&route);
  forget_calls ();
  report_pcm (&route);
  drain ();
  set_lg_codec ("ac3");
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (calls_matching ("setQualityAttribute"), ==, 1);
  g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 0);
  /* Still our relabel: restored once. */
  reset (&route);
  forget_calls ();
  report_pcm (&route);
  drain ();
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 1);

  /* No second report while one is in flight; once it ends the next is sent without a new query. */
  reset (&route);
  forget_calls ();
  gint64 start = g_get_monotonic_time ();
  report_pcm (&route); report_pcm (&route); report_pcm (&route);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 100 * G_TIME_SPAN_MILLISECOND);
  drain ();
  g_assert_cmpint (calls_matching ("getAudioInputProperty"), ==, 1);
  g_assert_cmpint (calls_matching ("\"codec\":\"pcm\""), ==, 1);
  report_pcm (&route);
  drain ();
  g_assert_cmpint (calls_matching ("getAudioInputProperty"), ==, 1);
  g_assert_cmpint (calls_matching ("\"codec\":\"pcm\""), ==, 2);
  lgroute_disarm (&route);
  drain ();

  /* Two threads disarming one route clean up once. */
  for (int round = 0; round < 20; round++) {
    reset (&route);
    TestSink *sink = (TestSink *) g_object_ref (route.sink);
    route.sinkpad = gst_pad_new ("racer", GST_PAD_SINK);
    forget_calls ();
    report_pcm (&route);
    drain ();
    g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 0);
    gint go = 0;
    Disarmer d = { &route, &go };
    GThread *a = g_thread_new ("disarm-a", disarm_thread, &d), *b = g_thread_new ("disarm-b", disarm_thread, &d);
    g_atomic_int_set (&go, 1);
    g_thread_join (a); g_thread_join (b);
    drain ();
    g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 1);
    g_assert_null (route.sink); g_assert_null (route.sinkpad);
    g_assert_false (sink->pcm);
    gst_object_unref (sink);
  }
}

int main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_assert_cmpint (g_mkdir_with_parents (LGROUTE_INSTANCES "/04260026", 0700), ==, 0);
  g_unlink (ROUTE_LOG);
  g_assert_nonnull (g_mkdtemp (luna_dir));
  g_snprintf (luna_send, sizeof luna_send, "%s/luna-send", luna_dir);
  /* The fake LG remembers the codec it was last given for _p1 (default dts). */
  gchar *script = g_strdup_printf ("#!/bin/sh\necho \"$5 $6\" >> %s/log\n"
      "case $5 in *getAudioInputProperty) sleep 0.3; c=$(cat %s/state 2>/dev/null || echo dts);"
      " echo '{\"returnValue\":true,\"audioInputProperty\":["
      "{\"codec\":\"ac3\",\"pipelineId\":\"_other\",\"immersive\":\"none\"},"
      "{\"codec\":\"'$c'\",\"pipelineId\":\"_p1\",\"immersive\":\"none\"}]}' ;;"
      " *) echo \"$6\" | sed 's/.*\"codec\":\"\\([a-z0-9]*\\)\".*/\\1/' > %s/state;"
      " echo '{\"returnValue\":true}' ;; esac\n", luna_dir, luna_dir, luna_dir);
  g_assert_true (g_file_set_contents (luna_send, script, -1, NULL));
  g_free (script);
  g_assert_cmpint (g_chmod (luna_send, 0700), ==, 0);
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
  /* The mixer state is read on every carrier buffer, not once per 250 ms. */
  g_assert_cmpint (checks, ==, 2);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (checks, ==, 3);
  /* The same carrier caps again (a resume resends them) keep the route. */
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_false (route.failed);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
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

  resume_tests ();
  restart_tests ();

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
  static const char pcm[] = "tv/setQualityAttribute {\"categoryType\":\"audioInputInfo\","
      "\"requestType\":\"setAudioInputInfo\",\"pipelineInfo\":{\"pipelineId\":\"_p1\","
      "\"resource\":\"ADEC1\",\"sourceType\":\"MEDIA\"},\"categoryInfo\":{\"codec\":\"pcm\","
      "\"channel\":\"8\",\"immersive\":\"none\"}}";
  static const char restore[] = "\"categoryInfo\":{\"codec\":\"dts\",\"immersive\":\"none\"}}";
  drain ();
  gchar *log = g_build_filename (luna_dir, "log", NULL);
  g_unlink (log); g_free (log);
  reset (&route);
  caps_event (&route); graph = after;
  g_assert_cmpint (luna_count (pcm), ==, 0);
  unmixed_result = 0;
  /* Reported once the bypass exists; the re-report for LG still mixing finds that one in flight. */
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); drain (); g_assert_cmpint (luna_count (pcm), ==, 1);
  route.next_check = g_get_monotonic_time () + 60 * G_TIME_SPAN_SECOND;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); drain (); g_assert_cmpint (luna_count (pcm), ==, 1);
  /* Still mixing at the next check: described again. */
  route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); drain (); g_assert_cmpint (luna_count (pcm), ==, 2);
  unmixed_result = 1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (luna_count (restore), ==, 1);
  g_assert_null (route.session);
  /* Values reach a JSON payload; quotes are refused. */
  g_assert_false (token ("_p1\",\"x")); g_assert_false (token ("")); g_assert_true (token ("ADEC1"));

  reset (&route); caps_event (&route); graph = after; receiver = FALSE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  reset (&route); graph_ok = FALSE;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  handoff_tests ();
  async_tests ();
  g_print ("LG route: bypass converter, carrier flags, CAPS changes and failure refusal pass\n");
  return 0;
}
