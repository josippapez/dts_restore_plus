#include <gst/gst.h>

static void
append_track (GString *line, GstCaps *caps)
{
  guint pid;

  if (caps && gst_caps_is_fixed (caps)) {
    const GstStructure *format = gst_caps_get_structure (caps, 0);

    if (gst_structure_get_uint (format, "pid", &pid)) {
      g_string_append_printf (line, "\t%04x\t%s", pid,
          gst_structure_get_name (format));
      return;
    }
  }
  g_string_append (line, "\t?\t?");
}

static void
print_collection (GstMessage *message)
{
  GstStreamCollection *collection;
  GString *line = g_string_new ("COLLECTION");

  gst_message_parse_stream_collection (message, &collection);
  for (guint i = 0; i < gst_stream_collection_get_size (collection); i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    GstCaps *caps = gst_stream_get_caps (stream);

    append_track (line, caps);
    gst_clear_caps (&caps);
  }
  g_print ("%s\n", line->str);
  g_string_free (line, TRUE);
  gst_object_unref (collection);
}

static void
capture_pad (GstElement *demux, GstPad *pad, gpointer output_directory)
{
  GstObject *pipeline = gst_object_get_parent (GST_OBJECT (demux));
  GstElement *sink = gst_element_factory_make ("filesink", NULL);
  const gchar *name = GST_PAD_NAME (pad);

  if (sink) {
    gchar *filename = g_build_filename (output_directory, name, NULL);

    /* The advertised AC-3 slot can reach EOS without any buffers. */
    g_object_set (sink, "location", filename, "async", FALSE, "sync", FALSE, NULL);
    g_free (filename);

    if (gst_bin_add (GST_BIN (pipeline), sink)) {
      GstPad *sink_pad = gst_element_get_static_pad (sink, "sink");
      GstPadLinkReturn linked = gst_pad_link (pad, sink_pad);

      gst_object_unref (sink_pad);
      if (linked == GST_PAD_LINK_OK && gst_element_sync_state_with_parent (sink)) {
        GstCaps *caps = gst_pad_get_current_caps (pad);
        GString *line = g_string_new ("PAD\t");

        g_string_append (line, name);
        append_track (line, caps);
        g_print ("%s\n", line->str);
        g_string_free (line, TRUE);
        gst_clear_caps (&caps);
        gst_object_unref (pipeline);
        return;
      }
    } else {
      gst_object_unref (sink);
    }
  }

  GST_ELEMENT_ERROR (demux, CORE, FAILED, ("Could not capture pad %s", name),
      (NULL));
  gst_object_unref (pipeline);
}

static gboolean
load_plugin (const gchar *filename)
{
  GError *error = NULL;
  GstPlugin *plugin = gst_plugin_load_file (filename, &error);

  if (!plugin) {
    g_printerr ("Could not load %s: %s\n", filename,
        error ? error->message : "no details");
    g_clear_error (&error);
    return FALSE;
  }
  gst_object_unref (plugin);
  return TRUE;
}

static gboolean
demux_from_plugin (GstElement *demux, const gchar *filename)
{
  GstElementFactory *factory = gst_element_get_factory (demux);
  GstPlugin *plugin = gst_plugin_feature_get_plugin (GST_PLUGIN_FEATURE (factory));
  const gchar *loaded = plugin ? gst_plugin_get_filename (plugin) : NULL;
  gboolean matches = g_strcmp0 (loaded, filename) == 0;

  if (!matches)
    g_printerr ("Expected tsdemux from %s, got %s\n", filename,
        loaded ? loaded : "no plugin file");
  gst_clear_object (&plugin);
  return matches;
}

static gboolean
wait_for_eos (GstElement *pipeline, GstElement *demux)
{
  GstBus *bus = gst_element_get_bus (pipeline);
  const GstMessageType types = GST_MESSAGE_EOS | GST_MESSAGE_ERROR |
      GST_MESSAGE_STREAM_COLLECTION;
  gboolean eos = FALSE;

  while (TRUE) {
    GstMessage *message = gst_bus_timed_pop_filtered (bus, 15 * GST_SECOND, types);
    if (!message) {
      g_printerr ("Timed out waiting for pipeline EOS\n");
      break;
    }
    GstMessageType type = GST_MESSAGE_TYPE (message);

    if (type == GST_MESSAGE_STREAM_COLLECTION &&
        GST_MESSAGE_SRC (message) == GST_OBJECT (demux))
      print_collection (message);
    if (type == GST_MESSAGE_ERROR) {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_printerr ("%s: %s\n%s\n", GST_MESSAGE_SRC_NAME (message), error->message,
          debug ? debug : "");
      g_error_free (error);
      g_free (debug);
    }
    gst_message_unref (message);
    if (type == GST_MESSAGE_EOS || type == GST_MESSAGE_ERROR) {
      eos = type == GST_MESSAGE_EOS;
      break;
    }
  }

  gst_object_unref (bus);
  return eos;
}

int
main (int argc, char **argv)
{
  guint major, minor, micro, nano;
  GstElement *pipeline, *source, *demux;
  gboolean ok = FALSE;

  if (argc != 5) {
    g_printerr ("Usage: %s <coreelements> <mpegtsdemux> <input> <output-dir>\n",
        argv[0]);
    return 1;
  }

  gst_init (NULL, NULL);
  gst_version (&major, &minor, &micro, &nano);
  if (major != 1 || minor != 24) {
    g_printerr ("Expected GStreamer 1.24, got %u.%u.%u\n", major, minor, micro);
    return 1;
  }
  g_print ("GStreamer %u.%u.%u\n", major, minor, micro);

  if (!load_plugin (argv[1]) || !load_plugin (argv[2])) {
    g_printerr ("Use the matching ARM LG GStreamer 1.24 libraries (--gst-libdir).\n");
    return 1;
  }

  pipeline = gst_pipeline_new (NULL);
  source = gst_element_factory_make ("filesrc", NULL);
  demux = gst_element_factory_make ("tsdemux", NULL);
  if (!pipeline || !source || !demux) {
    g_printerr ("Could not create filesrc/tsdemux pipeline\n");
    gst_clear_object (&source);
    gst_clear_object (&demux);
    gst_clear_object (&pipeline);
    return 1;
  }

  gst_bin_add_many (GST_BIN (pipeline), source, demux, NULL);
  g_object_set (source, "location", argv[3], NULL);
  g_signal_connect (demux, "pad-added", G_CALLBACK (capture_pad), argv[4]);
  if (!gst_element_link (source, demux)) {
    g_printerr ("Could not link filesrc to tsdemux\n");
  } else if (demux_from_plugin (demux, argv[2])) {
    if (gst_element_set_state (pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      g_printerr ("Could not start pipeline\n");
    else
      ok = wait_for_eos (pipeline, demux);
  }

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  return ok ? 0 : 1;
}
