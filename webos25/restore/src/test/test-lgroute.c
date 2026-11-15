#define _GNU_SOURCE
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define LGROUTE_RUN "/tmp/lgroute-test-run"
#define LGROUTE_INSTANCES "/tmp/lgroute-test-run/instances"
/* LG's renderer device is a plain file here; the kernel's answer is aren_tail. */
#define LGROUTE_AREN "/tmp/lgroute-test-run/aren0"
#define LGROUTE_SINK_WAIT (600 * G_TIME_SPAN_MILLISECOND)
#define LGROUTE_SNDOUT "/tmp/lgroute-test-run/sndout"
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
/* -1 for this many reads, then unmixed_result. */
static int unavailable_reads;
int lgroute_control_unmixed_why (char why[LGROUTE_WHY])
{
  checks++;
  int result = unavailable_reads > 0 ? (unavailable_reads--, -1) : unmixed_result;
  if (result < 0) g_strlcpy (why, "Sndout EARC OnOff is 0, not on", LGROUTE_WHY);
  return result;
}
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
/* AREN_GET_BYPASS_TAIL_UID; 0 fails like a kernel without it. */
static guint32 aren_tail;

static int
aren_ioctl (int fd, unsigned long request, ...)
{
  va_list args;
  (void) fd;
  g_assert_cmphex (request, ==, 0x80045223u);
  va_start (args, request);
  guint32 *tail = va_arg (args, guint32 *);
  va_end (args);
  if (!aren_tail) { errno = ENOTTY; return -1; }
  *tail = aren_tail;
  return 0;
}
#define ioctl aren_ioctl
#define g_file_get_contents read_or_graph
#define open open_property
#define write write_property
#define close close_property
#include "../lgroute.c"
#undef ioctl
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
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass), "test sink", "Sink/Audio", "stands in for LG's sink", "test");
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
  graph = before; graph_ok = receiver = writes_ok = TRUE; graph_reads = 0; unmixed_result = 1; unavailable_reads = 0;
  short_write = FALSE; aren_tail = 0;
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

  /* 9b. While the last window was slow the receiver gets silence, not a carrier
   * LG plays at the wrong speed: a tester heard white noise through slow
   * windows that never came three in a row. Normal pace brings the carrier back. */
  reset (&route);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  caps_event (&route); graph = after;
  carrier (&route, TRUE);
  route.window_start -= 2 * G_TIME_SPAN_SECOND;
  for (int i = 0; i < 24; i++) g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpuint (route.slow_windows, ==, 1);
  g_assert_true (last_silent);
  for (int i = 0; i < 24; i++) g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpuint (route.slow_windows, ==, 0);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  g_assert_false (route.failed);
  gst_element_set_state (route.sink, GST_STATE_NULL);
  lgroute_disarm (&route);

  /* 9c. The route log carries LG's own account of its ARC output, from the
   * [ARC] section only. The file has a C5's layout; the values are made up. */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT,
      "[SPK]\nOpen=1\nMute=1\nUnderflow=8\n\n[OPTIC]\nOutputCodec=PCM\nUnderflow=1\n\n"
      "[ARC]\nOpen=1\nArcOnOff=1\nArcMode=EARC\nOutputCodec=MAT\nOutputChannel=8\nGain=0x800000\n"
      "Mute=0\nDelay=0\nUnderflow=3\n\n[EARC]\nEArcOnOff=On\nUnderflow=0\n", -1, NULL));
  reset (&route);
  g_unlink (ROUTE_LOG);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  log = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &log, NULL, NULL));
  g_assert_nonnull (strstr (log, "eARC unmixed, carrier passes; LG output: ArcMode=EARC OutputCodec=MAT "
      "OutputChannel=8 Mute=0 Underflow=3\n"));
  g_free (log);
  g_unlink (LGROUTE_SNDOUT);
  lgroute_disarm (&route);

  /* 9d. LG's mixer word is its request; the kernel's status says which route
   * it took. A tester's TV log had LG's mixer taking the carrier as 8-channel
   * PCM, its queue full, while the word read unmixed: white noise. */
#define ARC_STATUS(codec) "[SPK]\nOpen=1\n\n[ARC]\nOpen=1\nArcMode=BYPASS\nOutputCodec=" codec \
    "\nOutputChannel=0\nMute=0\nUnderflow=0\n\n[EARC]\nEArcOnOff=On\n"
  g_unlink (BYPASS_SEEN);
  /* Until this TV has shown its bypass in that status, the status decides nothing. */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT, ARC_STATUS ("PCM"), -1, NULL));
  reset (&route);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  g_assert_false (g_file_test (BYPASS_SEEN, G_FILE_TEST_EXISTS));
  lgroute_disarm (&route);
  /* A play on LG's bypass route is remembered. */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT, ARC_STATUS ("Pass-Through"), -1, NULL));
  reset (&route);
  caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  g_assert_true (g_file_test (BYPASS_SEEN, G_FILE_TEST_EXISTS));
  /* From then on, a carrier LG keeps in its mixer goes out as silence... */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT, ARC_STATUS ("PCM"), -1, NULL));
  g_unlink (ROUTE_LOG);
  route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  g_assert_false (route.failed);
  /* ...comes back when LG takes the bypass route again... */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT, ARC_STATUS ("Pass-Through"), -1, NULL));
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent);
  /* ...and after a second of it the decoder takes over. */
  g_assert_true (g_file_set_contents (LGROUTE_SNDOUT, ARC_STATUS ("PCM"), -1, NULL));
  route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  route.mixed_since -= 2 * G_TIME_SPAN_SECOND;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpstr (route.reason, ==, "LG keeps the carrier in its mixer");
  log = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &log, NULL, NULL));
  g_assert_nonnull (strstr (log, "eARC unmixed but LG's mixer has the carrier, sending silence; LG output: "
      "ArcMode=BYPASS OutputCodec=PCM"));
  g_free (log);
  g_unlink (LGROUTE_SNDOUT);
  g_unlink (BYPASS_SEEN);
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

static gpointer add_sink_late (gpointer pipeline)
{
  g_usleep (150 * G_TIME_SPAN_MILLISECOND);
  gst_bin_add (GST_BIN (pipeline), gst_element_factory_make ("audiosink", NULL));
  return NULL;
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

  /* 14. A failed route leaves LG's sink alone until its disarm: a tester's TV
   * logged one turning LG's bypass on for the next track as its audio locked up. */
  reset (&route); forget_calls ();
  graph = after;
  wire (&route, &pad, &source);
  push_carrier (source);
  push_carrier (source);
  g_assert_true (((TestSink *) route.sink)->pcm);
  route_fail (&route, "eARC Pass Through unavailable");
  g_object_set (route.sink, "pcm-multi-ch", FALSE, NULL);
  gst_pad_push_event (source, gst_event_new_stream_start ("next"));
  g_assert_false (((TestSink *) route.sink)->pcm);
  unwire (&route, pad, source);
}

/* A track switch: the route arms on the new track's first frame while the
 * previous track's buffers are still queued in LG's playsink ahead of our caps
 * (a tester's G5: "armed on ADEC0", then "carrier arrived before renderer
 * caps" 8-22 ms later). Those buffers are not ours and pass untouched. */
static void stale_buffer_tests (void)
{
  LgRoute route;
  /* LG's sink pad has a template that takes PCM and the carrier; a templateless
   * pad would refuse the second caps (gst_pad_query_caps_default uses the current ones). */
  static GstStaticPadTemplate any_sink =
      GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
  reset (&route); forget_calls (); graph = after;
  GstPad *pad = gst_pad_new_from_static_template (&any_sink, "stale"), *source = gst_pad_new ("src", GST_PAD_SRC);
  gst_pad_set_event_function (pad, accept_event);
  gst_pad_set_chain_function (pad, accept_buffer);
  g_assert_true (gst_pad_set_active (pad, TRUE));
  g_assert_true (gst_pad_set_active (source, TRUE));
  g_assert_cmpint (gst_pad_link (source, pad), ==, GST_PAD_LINK_OK);
  /* The old track: decoded stereo PCM, flowing before the route exists. */
  GstCaps *pcm = gst_caps_from_string ("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  gst_pad_push_event (source, gst_event_new_stream_start ("old"));
  gst_pad_push_event (source, gst_event_new_caps (pcm));
  gst_pad_push_event (source, gst_event_new_segment (&segment));
  gst_caps_unref (pcm);
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 4096, NULL));
  /* The new track's decoder arms, as lgroute_arm does: the pad's caps are not the carrier. */
  route.waiting = TRUE;
  route.sinkpad = gst_object_ref (pad);
  route.events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, &route, NULL);
  route.buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, &route, NULL);
  g_assert_false (pad_has_carrier (pad));
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, after_route, NULL, NULL);
  int formats_before = formats, checks_before = checks;
  /* Two more old buffers drain from LG's queue: untouched, and the route still waits. */
  for (int i = 0; i < 2; i++) {
    GstBuffer *old = gst_buffer_new_allocate (NULL, 4096, NULL);
    gst_buffer_memset (old, 0, 0x5a, 4096);
    g_assert_cmpint (gst_pad_push (source, old), ==, GST_FLOW_OK);
    g_assert_cmpuint (pushed_byte, ==, 0x5a);
  }
  g_assert_false (route.failed); g_assert_false (route.configured);
  g_assert_cmpint (checks, ==, checks_before);
  g_assert_false (((TestSink *) route.sink)->pcm);
  /* The new track reaches the sink: stream id, carrier caps, then carrier. */
  GstCaps *carrier_caps = lgroute_carrier_caps ();
  gst_pad_push_event (source, gst_event_new_stream_start ("new"));
  gst_pad_push_event (source, gst_event_new_caps (carrier_caps));
  gst_pad_push_event (source, gst_event_new_segment (&segment));
  gst_caps_unref (carrier_caps);
  g_assert_true (((TestSink *) route.sink)->pcm);
  gst_element_set_state (route.sink, GST_STATE_PLAYING);
  push_carrier (source);
  g_assert_false (route.failed); g_assert_true (route.configured);
  g_assert_cmpint (formats, ==, formats_before + 1);
  g_assert_false (((TestSink *) route.sink)->pcm);   /* the first ES event left with the flag clear */
  /* LG's renderer still runs at the old track's pace (two testers' TVs, one a G3,
   * played every such carrier four times too slowly): it is restarted, once. */
  g_assert_true (route.restart_pending);
  /* Not at once: LG has just restarted the renderer itself for the new track.
   * A tester's TV froze twice, and once found no converter, when ours followed
   * 10-20 ms later. Until LG has settled the receiver gets silence. */
  push_carrier (source);
  g_assert_cmpuint (route.restarts, ==, 0);
  g_assert_true (route.restart_pending);
  g_assert_cmpuint (pushed_byte, ==, 0);
  route.restart_after = 0;
  push_carrier (source);
  g_assert_cmpuint (route.restarts, ==, 1);
  g_assert_false (route.failed); g_assert_true (route.configured);
  g_assert_cmpint (formats, ==, formats_before + 2);
  g_assert_false (((TestSink *) route.sink)->pcm);
  push_carrier (source);
  g_assert_true (((TestSink *) route.sink)->pcm);
  g_assert_false (route.restart_pending);
  g_assert_cmpuint (route.restarts, ==, 1);
  gchar *log = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &log, NULL, NULL));
  g_assert_nonnull (strstr (log, "earlier stream still draining, passed through"));
  g_assert_nonnull (strstr (log, "track switch: restarting LG's renderer once the sink plays"));
  g_free (log);
  unwire (&route, pad, source);

  /* A second switch inside LG's queue (Dolby for under a second, then back):
   * the Dolby track's own caps are still queued behind the first track's
   * buffers when the route arms. They are not a replacement of our carrier. */
  reset (&route); graph = after;
  pad = gst_pad_new_from_static_template (&any_sink, "stale2"); source = gst_pad_new ("src", GST_PAD_SRC);
  gst_pad_set_event_function (pad, accept_event);
  gst_pad_set_chain_function (pad, accept_buffer);
  g_assert_true (gst_pad_set_active (pad, TRUE));
  g_assert_true (gst_pad_set_active (source, TRUE));
  g_assert_cmpint (gst_pad_link (source, pad), ==, GST_PAD_LINK_OK);
  pcm = gst_caps_from_string ("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
  gst_pad_push_event (source, gst_event_new_stream_start ("first"));
  gst_pad_push_event (source, gst_event_new_caps (pcm));
  gst_pad_push_event (source, gst_event_new_segment (&segment));
  gst_caps_unref (pcm);
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 4096, NULL));
  route.waiting = TRUE;
  route.sinkpad = gst_object_ref (pad);
  route.events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, &route, NULL);
  route.buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, &route, NULL);
  gst_pad_push (source, gst_buffer_new_allocate (NULL, 4096, NULL));
  GstCaps *dolby = gst_caps_from_string ("audio/x-raw,format=S32LE,rate=48000,channels=10,layout=interleaved");
  gst_pad_push_event (source, gst_event_new_stream_start ("dolby"));
  gst_pad_push_event (source, gst_event_new_caps (dolby));
  gst_caps_unref (dolby);
  g_assert_cmpint (gst_pad_push (source, gst_buffer_new_allocate (NULL, 4096, NULL)), ==, GST_FLOW_OK);
  g_assert_false (route.failed); g_assert_nonnull (route.sink);
  carrier_caps = lgroute_carrier_caps ();
  gst_pad_push_event (source, gst_event_new_stream_start ("back"));
  gst_pad_push_event (source, gst_event_new_caps (carrier_caps));
  gst_caps_unref (carrier_caps);
  push_carrier (source);
  g_assert_false (route.failed); g_assert_true (route.configured);
  /* Once our caps are there, another format does replace the carrier. */
  pcm = gst_caps_from_string ("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
  gst_pad_push_event (source, gst_event_new_caps (pcm));
  gst_caps_unref (pcm);
  g_assert_true (route.failed); g_assert_null (route.sink);
  gst_pad_unlink (source, pad);
  gst_object_unref (source); gst_object_unref (pad);
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
  g_assert_true (lgroute_bypass_converter (after, NULL, uid));
  g_assert_cmpstr (uid, ==, "04260026");
  g_assert_false (lgroute_bypass_converter (before, NULL, uid));
  /* the main head is srcbin -> lipsync without an rts; it is never chosen */
  gchar *changed = replace_text (after, "rts_0425000a_000000001891f88e_out0 ->", "queue_0418000a_000000001891f88e_out0 ->");
  g_assert_false (lgroute_bypass_converter (changed, NULL, uid));
  g_free (changed);
  changed = replace_text (after, "}\n",
      "rts_042500ff_0000000000000001_out0 -> srcbin_042600ff_0000000000000002_in0;\n"
      "srcbin_042600ff_0000000000000001_out0 -> lipsync_042300ff_0000000000000002_in0;\n}\n");
  g_assert_false (lgroute_bypass_converter (changed, NULL, uid));
  g_assert_true (lgroute_bypass_converter (changed, "04230035", uid));
  g_assert_cmpstr (uid, ==, "04260026");
  g_assert_true (lgroute_bypass_converter (changed, "042300ff", uid));
  g_assert_cmpstr (uid, ==, "042600ff");
  g_assert_false (lgroute_bypass_converter (changed, "04230034", uid));
  g_free (changed);
  changed = replace_text (after, "rts_0425000a_000000001891f88e_out0 ->", "gluepw_08130001_out ->");
  g_assert_false (lgroute_bypass_converter (changed, NULL, uid));
  g_free (changed);
}

/* Another loaded pipeline's bypass chain in LG's graph. A tester's movie found
 * two after the app's sample player had left one, and decoded ("bypass
 * converter missing, ambiguous, or not configurable"). The renderer's own
 * descriptor says which chain is ours. /proc/self/fd is Linux's. */
static void ownership_tests (void)
{
#ifdef __linux__
  LgRoute route;
  gchar *two = replace_text (after, "}\n",
      "rts_042500ff_0000000000000001_out0 -> srcbin_042600ff_0000000000000002_in0;\n"
      "srcbin_042600ff_0000000000000001_out0 -> lipsync_042300ff_0000000000000002_in0;\n}\n");
  int device = open (LGROUTE_AREN, O_RDWR | O_CREAT, 0600);
  g_assert_cmpint (device, >=, 0);

  /* The kernel cannot say: two chains still refuse. */
  reset (&route); caps_event (&route); graph = two;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  /* It names our lipsync: the converter feeding it is the one configured
   * (write_property checks the path). */
  reset (&route); caps_event (&route); graph = two; aren_tail = 0x04230035;
  int formats_before = formats;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (route.configured); g_assert_cmpstr (route.converter, ==, "04260026");
  g_assert_cmpint (formats, ==, formats_before + 1);
  lgroute_disarm (&route);

  /* Ours is LG's ES bypass (queue -> lipsync), so LG built no PCM bypass for
   * this renderer: the one chain in the graph is the other pipeline's. */
  gchar *theirs = replace_text (after, "}\n",
      "queue_04180027_0000000000000001_out0 -> lipsync_04230029_0000000000000002_in0;\n}\n");
  reset (&route); caps_event (&route); graph = theirs; aren_tail = 0x04230029;
  formats_before = formats;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); g_assert_cmpint (formats, ==, formats_before);
  lgroute_disarm (&route);

  /* A number that is no lipsync in the graph says nothing: the one chain, as before. */
  reset (&route); caps_event (&route); graph = after; aren_tail = 0x12345678;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_cmpstr (route.converter, ==, "04260026");
  lgroute_disarm (&route);

  /* Two descriptors on the device: which is the sink's is not ours to guess. */
  int second = open (LGROUTE_AREN, O_RDWR);
  g_assert_cmpint (second, >=, 0);
  reset (&route); caps_event (&route); graph = two; aren_tail = 0x04230035;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  lgroute_disarm (&route);
  close (second); close (device);

  gchar *log = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &log, NULL, NULL));
  g_assert_nonnull (strstr (log, "configured converter 04260026 of this renderer"));
  g_assert_nonnull (strstr (log, "this renderer's bypass (lipsync 04230029) has no PCM converter"));
  g_free (log); g_free (two); g_free (theirs);
#endif
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
  g_unlink (ROUTE_LOG);
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (calls_matching ("setQualityAttribute"), ==, 1);
  g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 0);
  gchar *text = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
  g_assert_null (strstr (text, "restored"));
  g_free (text);
  /* Still our relabel: restored once, and the route log says so (testers'
   * failures sat at track switches, where the restore lands). */
  reset (&route);
  forget_calls ();
  report_pcm (&route);
  drain ();
  lgroute_disarm (&route);
  drain ();
  g_assert_cmpint (calls_matching ("\"codec\":\"dts\""), ==, 1);
  g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
  g_assert_nonnull (strstr (text, "restored ADEC1's label at LG to codec \"dts\", immersive \"none\": accepted\n"));
  g_free (text);

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
  stale_buffer_tests ();
  ownership_tests ();

  reset (&route); caps_event (&route); graph = after; short_write = TRUE;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);

  /* eARC unavailable gets the second a mix gets: a tester's TrueHD route failed
   * on one such reading as LG switched tracks. The log names what was read. */
  reset (&route); caps_event (&route); graph = after;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_unlink (ROUTE_LOG);
  unmixed_result = -1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); g_assert_false (route.failed);
  unmixed_result = 1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (last_silent); g_assert_cmpint (route.mixed_since, ==, 0);
  unmixed_result = -1;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent);
  route.mixed_since -= G_TIME_SPAN_SECOND;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed);
  g_assert_cmpstr (route.reason, ==, "eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on)");
  {
    gchar *text = NULL;
    g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
    g_assert_nonnull (strstr (text, "eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on), sending silence"));
    g_assert_nonnull (strstr (text, "failed, decoding instead: eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on)\n"));
    g_free (text);
  }
  lgroute_disarm (&route);

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

  /* The preroll wait is not LG mixing for a second: a G2 configured its DTS-HD
   * route at 00:11:51.245, got its next buffer at 00:11:52.975 and gave up on
   * it. That buffer starts the second, and describes the carrier again. */
  reset (&route); forget_calls (); caps_event (&route); graph = after; unmixed_result = 0;
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  drain ();
  route.mixed_since -= 1730 * G_TIME_SPAN_MILLISECOND; route.last_push -= 1730 * G_TIME_SPAN_MILLISECOND;
  route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_OK);
  g_assert_false (route.failed); g_assert_true (last_silent);
  drain (); g_assert_cmpint (calls_matching ("\"codec\":\"pcm\""), ==, 2);
  route.mixed_since -= G_TIME_SPAN_SECOND; route.next_check = 0;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_cmpstr (route.reason, ==, "LG keeps eARC mixing on");
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
  gchar *reply = NULL;
  g_assert_true (g_file_get_contents (ROUTE_LOG, &reply, NULL, NULL));
  g_assert_nonnull (strstr (reply, "described ADEC1 to LG as 8-channel PCM: accepted"));
  g_free (reply);
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
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_OK);
  g_assert_true (last_silent); g_assert_false (route.failed);
  route.mixed_since -= G_TIME_SPAN_SECOND;
  g_assert_cmpint (carrier (&route, FALSE), ==, GST_PAD_PROBE_DROP);
  g_assert_cmpstr (route.reason, ==, "eARC Pass Through unavailable (sound output is not a receiver)");
  lgroute_disarm (&route);

  reset (&route); graph_ok = FALSE;
  g_assert_cmpint (caps_event (&route), ==, GST_PAD_PROBE_OK);
  g_assert_cmpint (carrier (&route, TRUE), ==, GST_PAD_PROBE_DROP);
  g_assert_true (route.failed); lgroute_disarm (&route);
  /* A track that gave up refuses to arm and the route log says why; a route
   * reset for the next track (dtsdec does this on a switch) checks again. */
  {
    LgRoute fresh;
    lgroute_init (&fresh, NULL);
    fresh.failed = TRUE;
    fresh.reason = "LG plays the carrier too slowly";
    g_assert_false (lgroute_arm (&fresh));
    lgroute_init (&fresh, NULL);
    g_assert_false (fresh.failed);
    receiver = FALSE;
    g_assert_false (lgroute_arm (&fresh));
    receiver = TRUE;
    gchar *refusals = NULL;
    g_assert_true (g_file_get_contents (ROUTE_LOG, &refusals, NULL, NULL));
    g_assert_nonnull (strstr (refusals, "not armed: gave up earlier on this track (LG plays the carrier too slowly)"));
    g_assert_nonnull (strstr (refusals, "not armed: sound output is not a receiver"));
    g_free (refusals);
  }
  /* No LG sink: the route log says what the pipeline holds and what state it is in. */
  {
    GstElement *pipeline = gst_pipeline_new ("player"), *decoder = gst_bin_new ("decoder");
    GstElement *other = gst_element_factory_make ("fakesink", NULL);
    g_assert_nonnull (other);
    gst_bin_add_many (GST_BIN (pipeline), decoder, other, NULL);
    LgRoute fresh;
    lgroute_init (&fresh, decoder);
    g_assert_false (lgroute_arm (&fresh));
    gchar *refusal = NULL;
    g_assert_true (g_file_get_contents (ROUTE_LOG, &refusal, NULL, NULL));
    g_assert_nonnull (strstr (refusal, "not armed: 0 verified LG sinks in the pipeline "
        "(0 audiosink elements; player is NULL, going VOID_PENDING; sinks: fakesink)"));
    g_free (refusal);
    gst_object_unref (pipeline);
  }
  /* The first frame can come before playsink has built its audio chain (a
   * tester's DTS:X opened as core DTS; a G5's sink came 100-300 ms later):
   * arming waits for an audiosink instead of refusing at once. */
  {
    GstElement *pipeline = gst_pipeline_new ("player"), *decoder = gst_bin_new ("decoder");
    gst_bin_add (GST_BIN (pipeline), decoder);
    g_assert_true (gst_element_register (NULL, "audiosink", GST_RANK_NONE, test_sink_get_type ()));
    LgRoute fresh;
    lgroute_init (&fresh, decoder);
    /* Not while the pipeline is closing: a tester's log had the wait run for
     * 1.75 s after a file was closed ("playbin3-0 is READY, going NULL"). */
    gint64 start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&fresh));
    g_assert_cmpint (g_get_monotonic_time () - start, <, 100 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
    GThread *late = g_thread_new ("late-sink", add_sink_late, pipeline);
    start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&fresh));   /* found, but not LG's verified build */
    gint64 waited = g_get_monotonic_time () - start;
    g_thread_join (late);
    g_assert_cmpint (waited, >=, 100 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint (waited, <, 500 * G_TIME_SPAN_MILLISECOND);
    gchar *refusal = NULL;
    g_assert_true (g_file_get_contents (ROUTE_LOG, &refusal, NULL, NULL));
    g_assert_nonnull (strstr (refusal, "(1 audiosink elements; player is"));
    g_free (refusal);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
  }
  /* A tester's TrueHD to DTS switch read eARC unavailable once and the DTS track
   * stayed core only and silent. At a track switch (LG's sink already has caps)
   * eARC is read again for up to 500 ms; a first play does not wait. */
  {
    static GstStaticPadTemplate any =
        GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
    GstElement *pipeline = gst_pipeline_new ("player"), *decoder = gst_bin_new ("decoder");
    GstElement *sink = gst_element_factory_make ("audiosink", NULL);
    GstPad *input = gst_pad_new_from_static_template (&any, "sink"), *feed = gst_pad_new ("feed", GST_PAD_SRC);
    gst_pad_set_event_function (input, accept_event);
    gst_element_add_pad (sink, input);
    gst_bin_add_many (GST_BIN (pipeline), decoder, sink, NULL);
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
    g_assert_true (gst_pad_set_active (feed, TRUE));
    g_assert_cmpint (gst_pad_link (feed, input), ==, GST_PAD_LINK_OK);
    LgRoute fresh;
    lgroute_init (&fresh, decoder);
    gchar *text = NULL;

    /* First play: no caps on LG's sink, refused at once, with the reason. */
    g_unlink (ROUTE_LOG);
    unmixed_result = -1;
    gint64 start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&fresh));
    g_assert_cmpint (g_get_monotonic_time () - start, <, 100 * G_TIME_SPAN_MILLISECOND);
    g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
    g_assert_nonnull (strstr (text, "not armed: eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on)\n"));
    g_assert_null (strstr (text, "track switch"));
    g_free (text);

    /* The earlier track played: a passing reading is waited out. */
    GstCaps *pcm = gst_caps_from_string ("audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved");
    gst_pad_push_event (feed, gst_event_new_stream_start ("old"));
    gst_pad_push_event (feed, gst_event_new_caps (pcm));
    gst_caps_unref (pcm);
    g_unlink (ROUTE_LOG);
    unmixed_result = 1; unavailable_reads = 3;
    start = g_get_monotonic_time ();
    lgroute_arm (&fresh);   /* goes on to the sink, which is not LG's verified build */
    gint64 waited = g_get_monotonic_time () - start;
    g_assert_cmpint (waited, >=, 100 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint (waited, <, 400 * G_TIME_SPAN_MILLISECOND);
    g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
    g_assert_nonnull (strstr (text, "track switch: eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on), "
        "read again for "));
    g_assert_nonnull (strstr (text, " ms: unmixed\n"));
    g_assert_null (strstr (text, "not armed: eARC"));
    g_assert_nonnull (strstr (text, "not armed: 0 verified LG sinks"));
    g_free (text);

    /* One that lasts gives up after 500 ms. */
    g_unlink (ROUTE_LOG);
    unmixed_result = -1;
    start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&fresh));
    waited = g_get_monotonic_time () - start;
    g_assert_cmpint (waited, >=, 500 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint (waited, <, 700 * G_TIME_SPAN_MILLISECOND);
    g_assert_true (g_file_get_contents (ROUTE_LOG, &text, NULL, NULL));
    g_assert_nonnull (strstr (text, " ms: unavailable (Sndout EARC OnOff is 0, not on)\n"));
    g_assert_nonnull (strstr (text, "not armed: eARC Pass Through unavailable (Sndout EARC OnOff is 0, not on)\n"));
    g_free (text);

    /* Not while the pipeline closes. */
    GST_STATE_TARGET (pipeline) = GST_STATE_READY;
    start = g_get_monotonic_time ();
    g_assert_false (lgroute_arm (&fresh));
    g_assert_cmpint (g_get_monotonic_time () - start, <, 100 * G_TIME_SPAN_MILLISECOND);
    GST_STATE_TARGET (pipeline) = GST_STATE_PAUSED;

    unmixed_result = 1;
    gst_pad_unlink (feed, input);
    gst_object_unref (feed);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);
  }
  handoff_tests ();
  async_tests ();
  g_print ("LG route: bypass converter, carrier flags, CAPS changes and failure refusal pass\n");
  return 0;
}
