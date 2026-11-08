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
/* Flushes are not GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM; a seek or resume sends them. */
#define EVENT_PROBE_MASK (GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH)

GST_DEBUG_CATEGORY_STATIC (lgroute_debug);
#define GST_CAT_DEFAULT (lgroute_debug)
#ifndef LUNA_SEND
#define LUNA_SEND "/usr/bin/luna-send"
#endif

/* luna-send blocks for up to 2 s, so it never runs on a streaming thread: one
 * ordered worker sends every request. A request owns copies of what it needs
 * and shares only the session, so it never touches an LgRoute. */
struct LgSession {
  gint refs, reporting;
  gchar pipeline[64], codec[32], immersive[16];   /* LG's own report, written by the worker only */
};

typedef struct {
  LgSession *session;
  gboolean restore;
  gchar resource[16];
} Request;

/* Guards the LgRoute fields that name what is armed (sink, sinkpad, probe ids,
 * session, resource), and the lazy worker. Never held across a GStreamer or luna call. */
static GMutex route_lock;
static GThreadPool *worker_pool;
static gint outstanding;

static void session_unref (LgSession *session)
{
  if (g_atomic_int_dec_and_test (&session->refs))
    g_free (session);
}

/* One luna-send call; TRUE when LG replies returnValue:true. */
static gboolean luna (const gchar *uri, const gchar *payload, gchar **reply)
{
  const gchar *argv[] = { LUNA_SEND, "-n", "1", "-w", "2000", uri, payload, NULL };
  /* LG's LD_PRELOAD in the media pipeline breaks luna-send. */
  gchar **env = g_environ_unsetenv (g_get_environ (), "LD_PRELOAD"), *out = NULL;
  gboolean ok = g_spawn_sync (NULL, (gchar **) argv, env, G_SPAWN_STDERR_TO_DEV_NULL,
      NULL, NULL, &out, NULL, NULL, NULL) && out && strstr (out, "\"returnValue\":true");
  g_strfreev (env);
  if (reply) *reply = out; else g_free (out);
  return ok;
}

/* Values reach a JSON payload; anything beyond [A-Za-z0-9_.-] is refused. */
static gboolean token (const gchar *value)
{
  return value && *value && !value[strspn (value,
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")];
}

/* audiod forwards this to UMI, which reapplies the digital output policy. */
static gboolean input_set (const gchar *pipeline, const gchar *resource, const gchar *codec,
    const gchar *channel, const gchar *immersive)
{
  if (!token (pipeline) || !token (resource) || !token (codec)
      || !token (immersive) || (channel && !token (channel)))
    return FALSE;
  gchar *payload = g_strdup_printf ("{\"categoryType\":\"audioInputInfo\","
      "\"requestType\":\"setAudioInputInfo\",\"pipelineInfo\":{\"pipelineId\":\"%s\","
      "\"resource\":\"%s\",\"sourceType\":\"MEDIA\"},\"categoryInfo\":{\"codec\":\"%s\"%s%s%s,"
      "\"immersive\":\"%s\"}}", pipeline, resource, codec,
      channel ? ",\"channel\":\"" : "", channel ? channel : "", channel ? "\"" : "", immersive);
  gboolean ok = luna ("luna://com.webos.service.audio/tv/setQualityAttribute", payload, NULL);
  g_free (payload);
  return ok;
}

static gboolean field (const gchar *object, const gchar *key, gchar *value, gsize size)
{
  gchar *pattern = g_strdup_printf ("\"%s\":\"", key);
  const gchar *start = strstr (object, pattern), *stop;
  gsize length = strlen (pattern);
  g_free (pattern);
  if (!start || !(stop = strchr (start + length, '"')) || (gsize) (stop - start - length) >= size)
    return FALSE;
  g_strlcpy (value, start + length, stop - start - length + 1);
  return TRUE;
}

/* This pipeline's id (from -scom.webos.pipeline.<id>) and LG's own report for it. */
static gboolean input_query (LgSession *session)
{
  static const gchar prefix[] = "-scom.webos.pipeline.";
  gchar *args = NULL, *reply = NULL;
  gsize length = 0;
  if (!g_file_get_contents ("/proc/self/cmdline", &args, &length, NULL))
    return FALSE;
  for (gchar *arg = args; arg < args + length; arg += strlen (arg) + 1)
    if (g_str_has_prefix (arg, prefix))
      g_strlcpy (session->pipeline, arg + sizeof prefix - 1, sizeof session->pipeline);
  g_free (args);
  gchar *key = g_strdup_printf ("\"pipelineId\":\"%s\"", session->pipeline);
  gboolean ok = token (session->pipeline)
      && luna ("luna://com.webos.service.audio/tv/getAudioInputProperty", "{}", &reply);
  gchar *match = ok ? strstr (reply, key) : NULL, *object = match, *end;
  g_free (key);
  while (object && object > reply && *object != '{')
    object--;
  if ((end = match ? strchr (match, '}') : NULL))
    *end = 0;
  ok = end && field (object, "codec", session->codec, sizeof session->codec);
  if (ok && !field (object, "immersive", session->immersive, sizeof session->immersive))
    g_strlcpy (session->immersive, "none", sizeof session->immersive);
  g_free (reply);
  if (!ok) session->pipeline[0] = 0;
  return ok;
}

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

static void run_request (gpointer data, gpointer unused)
{
  Request *request = data;
  LgSession *session = request->session;
  (void) unused;
  if (request->restore) {
    /* The restore now lands after the caller moved on: if LG already reports
     * something other than our relabel, it has described the next stream and
     * the saved codec would overwrite it. */
    LgSession now = { 0 };
    if (!session->pipeline[0])
      ;
    else if (input_query (&now) && g_strcmp0 (now.codec, "pcm"))
      g_debug ("lgroute: LG reports \"%s\", not restoring \"%s\"", now.codec, session->codec);
    else
      input_set (session->pipeline, request->resource, session->codec, NULL, session->immersive);
  } else {
    if (session->pipeline[0] || input_query (session))
      input_set (session->pipeline, request->resource, "pcm", "8", "none");
    g_atomic_int_set (&session->reporting, 0);
  }
  session_unref (session);
  g_free (request);
  g_atomic_int_dec_and_test (&outstanding);
}

/* Takes the caller's session reference. */
static void queue_request (LgSession *session, const gchar *resource, gboolean restore)
{
  Request *request = g_new0 (Request, 1);
  request->session = session;
  request->restore = restore;
  g_strlcpy (request->resource, resource, sizeof request->resource);
  g_mutex_lock (&route_lock);
  if (!worker_pool)
    worker_pool = g_thread_pool_new (run_request, NULL, 1, FALSE, NULL);
  g_atomic_int_inc (&outstanding);
  gboolean ok = g_thread_pool_push (worker_pool, request, NULL);
  g_mutex_unlock (&route_lock);
  if (!ok) {
    if (!restore) g_atomic_int_set (&session->reporting, 0);
    g_atomic_int_dec_and_test (&outstanding);
    session_unref (session);
    g_free (request);
  }
}

/* LG mixes any codec missing from the receiver's short eARC list (DTS on most
 * soundbars); 8-channel PCM matches, so LG itself writes the unmixed output.
 * Asynchronous; while one report is in flight the next one is dropped. */
static void report_pcm (LgRoute *route)
{
  gchar resource[sizeof route->resource];
  g_mutex_lock (&route_lock);
  LgSession *session = route->session;
  if (!route->resource[0]) {
    g_mutex_unlock (&route_lock);
    return;
  }
  if (!session) {
    session = route->session = g_new0 (LgSession, 1);
    session->refs = 1;
  }
  g_atomic_int_inc (&session->refs);
  g_strlcpy (resource, route->resource, sizeof resource);
  g_mutex_unlock (&route_lock);
  if (g_atomic_int_compare_and_exchange (&session->reporting, 0, 1))
    queue_request (session, resource, FALSE);
  else
    session_unref (session);
}

/* The sink is read under the lock but driven outside it, so a disarm on another
 * thread can neither free it under us nor wait on a pad we are blocked in. */
static void set_pcm (LgRoute *route, gboolean on)
{
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink ? gst_object_ref (route->sink) : NULL;
  g_mutex_unlock (&route_lock);
  if (!sink)
    return;
  g_object_set (sink, "pcm-multi-ch", on, NULL);
  gst_object_unref (sink);
}

static GstPadProbeReturn silence (GstPadProbeInfo *info)
{
  GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
  if (!buffer)
    return GST_PAD_PROBE_DROP;
  GST_PAD_PROBE_INFO_DATA (info) = buffer;
  gst_buffer_memset (buffer, 0, 0, gst_buffer_get_size (buffer));
  return GST_PAD_PROBE_OK;
}

/* Every give-up goes through here so a tester's log names the reason. */
static void route_fail (LgRoute *route, const gchar *reason)
{
  route->failed = TRUE;
  route->reason = reason;
  GST_WARNING_OBJECT (route->element, "lgroute: HD transport failed: %s", reason);
}

static GstPadProbeReturn event_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  (void) pad;
  GST_LOG_OBJECT (route->element, "lgroute: event %s (configured=%d unmixed=%d)",
      GST_EVENT_TYPE_NAME (event), route->configured, route->unmixed);
  if (GST_EVENT_TYPE (event) == GST_EVENT_STREAM_START) {
    const gchar *id = NULL;
    gst_event_parse_stream_start (event, &id);
    if (!id)
      id = "";
    if (!route->configured) {
      g_strlcpy (route->stream_id, id, sizeof route->stream_id);
    } else if (route->stream_id[0] && !g_strcmp0 (route->stream_id, id)) {
      /* LG's sink rebuilds its renderer only when the id changes. */
      GST_INFO_OBJECT (route->element, "lgroute: repeated STREAM_START \"%s\", route kept", id);
    } else {
      /* The sink restarts the renderer (close, open, config), which re-arms its
       * FIRST event and drops our converter setup: redo both for this stream.
       * Carrier data waits for the new caps and a fresh mixer reading. */
      GST_INFO_OBJECT (route->element, "lgroute: stream id \"%s\" -> \"%s\", renderer "
          "will restart, redoing route setup", route->stream_id, id);
      g_strlcpy (route->stream_id, id, sizeof route->stream_id);
      route->configured = route->first = route->feeding = route->unmixed = FALSE;
      route->next_check = route->mixed_since = 0;
    }
    return GST_PAD_PROBE_OK;
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    /* Seeks and resumes flush; the next carrier buffer must see LG's current mixer state. */
    GST_INFO_OBJECT (route->element, "lgroute: FLUSH_STOP, mixer state re-read before next carrier");
    route->next_check = 0;
    route->unmixed = FALSE;
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
    GstCaps *caps = NULL, *carrier = lgroute_carrier_caps ();
    gst_event_parse_caps (event, &caps);
    gboolean is_carrier = gst_caps_is_equal (caps, carrier);
    gst_caps_unref (carrier);
    if (!is_carrier) {
      route_fail (route, "HD transport replaced by another format");
      g_mutex_lock (&route_lock);
      route->events = 0;   /* this probe removes itself */
      g_mutex_unlock (&route_lock);
      lgroute_disarm (route);
      return GST_PAD_PROBE_REMOVE;
    }
    if (route->configured) {
      /* Identical caps do not make LG's sink reopen the renderer. */
      GST_INFO_OBJECT (route->element, "lgroute: repeated carrier CAPS, route kept");
      return GST_PAD_PROBE_OK;
    }
    GST_INFO_OBJECT (route->element, "lgroute: carrier CAPS, renderer setup pending");
    route->first = TRUE;
    route->feeding = FALSE;
    set_pcm (route, TRUE);
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  LgRoute *route = data;
  (void) pad;
  if (route->failed)
    return GST_PAD_PROBE_DROP;
  if (!route->configured) {
    if (!route->first) {
      route_fail (route, "carrier arrived before renderer caps");
      return GST_PAD_PROBE_DROP;
    }
    route->first = FALSE;
    /* The first ES event must leave with the flag clear. */
    set_pcm (route, FALSE);
    route->configured = configure_converter ();
    if (!route->configured) {
      route_fail (route, "bypass converter missing, ambiguous, or not configurable");
      return GST_PAD_PROBE_DROP;
    }
    GST_INFO_OBJECT (route->element, "lgroute: bypass converter configured, first carrier buffer");
    /* The kernel routes the bypass to eARC only if it exists when LG writes the unmixed output. */
    report_pcm (route);
  } else {
    GstBuffer *buffer = gst_buffer_make_writable (GST_PAD_PROBE_INFO_BUFFER (info));
    if (!buffer)
      return GST_PAD_PROBE_DROP;
    GST_PAD_PROBE_INFO_DATA (info) = buffer;
    GST_BUFFER_FLAG_UNSET (buffer, ES_FLAG);
    if (!route->feeding) {
      set_pcm (route, TRUE);
      route->feeding = TRUE;
    }
  }
  gint64 now = g_get_monotonic_time ();
  gboolean due = now >= route->next_check, was_unmixed = route->unmixed;
  /* Read on every buffer: LG can switch eARC back to mixed at any time, and a mixed
   * eARC plays the carrier as noise. It is one cheap ALSA ioctl read. */
  int unmixed = lgroute_control_receiver () ? lgroute_control_unmixed () : -1;
  if (unmixed > 0)
    route->mixed_since = 0;
  else if (!route->mixed_since && unmixed == 0)
    route->mixed_since = now;
  /* LG's pipeline can re-report the real codec; describe the carrier again. */
  if (unmixed == 0 && due)
    report_pcm (route);
  route->unmixed = unmixed > 0;
  if (route->unmixed != was_unmixed)
    GST_INFO_OBJECT (route->element, "lgroute: eARC now %s (reading %d)",
        route->unmixed ? "unmixed, carrier passes" : "mixed or unavailable, sending silence", unmixed);
  /* LG's policy rewrites can mix briefly; only a lasting mix means TV sound overlays the carrier. */
  if (unmixed < 0
      || (route->mixed_since && now - route->mixed_since >= G_TIME_SPAN_SECOND)) {
    route_fail (route, unmixed < 0 ? "eARC Pass Through unavailable" : "LG keeps eARC mixing on");
    return GST_PAD_PROBE_DROP;
  }
  if (due)
    route->next_check = now + 250 * G_TIME_SPAN_MILLISECOND;
  /* Mixed eARC would play the carrier as noise; send silence until LG unmixes. */
  return route->unmixed ? GST_PAD_PROBE_OK : silence (info);
}

void lgroute_init (LgRoute *route, GstElement *element)
{
  static gsize category;
  if (g_once_init_enter (&category)) {
    GST_DEBUG_CATEGORY_INIT (lgroute_debug, "dtsdec-lgroute", 0, "LG eARC HD transport");
    g_once_init_leave (&category, 1);
  }
  memset (route, 0, sizeof *route);
  route->element = element;
}

gboolean lgroute_arm (LgRoute *route)
{
  g_mutex_lock (&route_lock);
  gboolean armed = route->sink != NULL;
  g_mutex_unlock (&route_lock);
  if (armed)
    return !route->failed;
  if (route->failed || !lgroute_control_receiver () || lgroute_control_unmixed () < 0)
    return FALSE;
  GstElement *sink = NULL;
  GstObject *top = gst_object_ref (route->element), *parent;
  while ((parent = gst_object_get_parent (top))) {
    gst_object_unref (top);
    top = parent;
  }
  unsigned count = 0;
  if (GST_IS_BIN (top)) {
    GstIterator *iterator = gst_bin_iterate_all_by_element_factory_name (GST_BIN (top), "audiosink");
    GValue value = G_VALUE_INIT;
    for (GstIteratorResult result; (result = gst_iterator_next (iterator, &value)) != GST_ITERATOR_DONE
        && result != GST_ITERATOR_ERROR;) {
      if (result == GST_ITERATOR_RESYNC) {
        gst_iterator_resync (iterator);
        gst_clear_object (&sink);
        count = 0;
        continue;
      }
      GstElement *element = g_value_get_object (&value);
      if (verified_sink (element) && !count++)
        sink = gst_object_ref (element);
      g_value_reset (&value);
    }
    g_value_unset (&value);
    gst_iterator_free (iterator);
  }
  gst_object_unref (top);
  if (count != 1 || !sink) {
    route->reason = "no unique verified LG sink in pipeline";
    gst_clear_object (&sink);
    return FALSE;
  }
  GstPad *pad = gst_element_get_static_pad (sink, "sink");
  if (!pad) { gst_object_unref (sink); return FALSE; }
  gchar resource[sizeof route->resource] = "";
  GObjectClass *klass = G_OBJECT_GET_CLASS (sink);
  if (g_object_class_find_property (klass, "index") && g_object_class_find_property (klass, "mixer")) {
    guint index = 0;
    gboolean mixer = FALSE;
    g_object_get (sink, "index", &index, "mixer", &mixer, NULL);
    g_snprintf (resource, sizeof resource, "%s%u", mixer ? "AMIXER" : "ADEC", index);
  }
  /* The probes run at once on sticky events, so the route is published first. */
  g_mutex_lock (&route_lock);
  route->sink = sink;
  route->sinkpad = pad;
  g_strlcpy (route->resource, resource, sizeof route->resource);
  g_mutex_unlock (&route_lock);
  gulong events = gst_pad_add_probe (pad, EVENT_PROBE_MASK, event_probe, route, NULL);
  gulong buffers = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe, route, NULL);
  g_mutex_lock (&route_lock);
  route->events = events;
  route->buffers = buffers;
  g_mutex_unlock (&route_lock);
  /* A replay re-arms on a pad that already holds the carrier caps and may not resend them. */
  GstCaps *current = gst_pad_get_current_caps (pad), *carrier = lgroute_carrier_caps ();
  if (current && gst_caps_is_equal (current, carrier)) {
    route->first = TRUE;
    route->feeding = FALSE;
    set_pcm (route, TRUE);
  }
  gst_caps_unref (carrier);
  if (current) gst_caps_unref (current);
  return TRUE;
}

/* Callable from several threads at once (the sink's and the decoder's streaming
 * threads, an old decoder's teardown): the first caller takes everything, so
 * the probes are removed and LG's codec restored exactly once. */
void lgroute_disarm (LgRoute *route)
{
  gchar resource[sizeof route->resource];
  g_mutex_lock (&route_lock);
  GstElement *sink = route->sink;
  GstPad *pad = route->sinkpad;
  LgSession *session = route->session;
  gulong events = route->events, buffers = route->buffers;
  g_strlcpy (resource, route->resource, sizeof resource);
  route->sink = NULL;
  route->sinkpad = NULL;
  route->session = NULL;
  route->events = route->buffers = 0;
  route->mixed_since = 0;
  route->first = route->feeding = route->configured = route->unmixed = FALSE;
  route->stream_id[0] = 0;
  route->resource[0] = 0;
  g_mutex_unlock (&route_lock);
  if (pad) {
    if (events) gst_pad_remove_probe (pad, events);
    if (buffers) gst_pad_remove_probe (pad, buffers);
  }
  if (sink)
    g_object_set (sink, "pcm-multi-ch", FALSE, NULL);
  /* Queued behind any report for this pipeline; it reads LG's codec from the session. */
  if (session)
    queue_request (session, resource, TRUE);
  gst_clear_object (&pad);
  gst_clear_object (&sink);
}
