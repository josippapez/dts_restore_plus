#include <gst/gst.h>

typedef struct {
  GChecksum *checksum;
  guint bursts;
} Capture;

static void
link_truehd (GstElement * demux, GstPad * source, gpointer user_data)
{
  GstCaps *caps = gst_pad_get_current_caps (source);
  GstPad *sink = gst_element_get_static_pad (user_data, "sink");

  if (caps && gst_structure_has_name (gst_caps_get_structure (caps, 0),
          "audio/x-true-hd"))
    g_assert_cmpint (gst_pad_link (source, sink), ==, GST_PAD_LINK_OK);
  gst_object_unref (sink);
  if (caps)
    gst_caps_unref (caps);
}

static GstPadProbeReturn
capture_burst (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Capture *capture = user_data;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  GstMapInfo map;
  GstCaps *caps = gst_pad_get_current_caps (pad);
  const GstStructure *structure = gst_caps_get_structure (caps, 0);
  gint rate = 0, channels = 0;

  g_assert_true (gst_structure_get_int (structure, "rate", &rate));
  g_assert_true (gst_structure_get_int (structure, "channels", &channels));
  g_assert_cmpint (rate, ==, 192000);
  g_assert_cmpint (channels, ==, 8);
  gst_caps_unref (caps);
  g_assert_true (GST_BUFFER_FLAG_IS_SET (buffer, (1u << 21)));
  g_assert_true (gst_buffer_map (buffer, &map, GST_MAP_READ));
  g_assert_cmpuint (map.size, ==, 61440);
  g_assert_cmphex (GST_READ_UINT32_BE (map.data), ==, 0xf8724e1f);
  g_assert_cmphex (GST_READ_UINT16_BE (map.data + 4), ==, 0x16);
  g_checksum_update (capture->checksum, map.data, map.size);
  capture->bursts++;
  gst_buffer_unmap (buffer, &map);
  return GST_PAD_PROBE_OK;
}

int
main (int argc, char **argv)
{
  GstElement *pipeline, *source, *sink, *demux, *decoder;
  GstPad *pad;
  GstBus *bus;
  GstMessage *message;
  GError *error = NULL;
  Capture capture = { g_checksum_new (G_CHECKSUM_SHA256), 0 };
  gchar *expected = NULL;
  guint expected_bursts = 0, round;

  gst_init (&argc, &argv);
  if (argc != 3) {
    g_printerr ("usage: %s <matroskademux|tsdemux> <TrueHD sample>\n", argv[0]);
    return 2;
  }
  g_assert_true (!g_strcmp0 (argv[1], "matroskademux")
      || !g_strcmp0 (argv[1], "tsdemux"));
    gchar *description = g_strdup_printf ("filesrc name=input ! %s name=demux "
      "adecswitch name=decoder ! fakesink name=output sync=false", argv[1]);
  pipeline = gst_parse_launch (description, &error);
  g_free (description);
  g_assert_no_error (error);
  demux = gst_bin_get_by_name (GST_BIN (pipeline), "demux");
  decoder = gst_bin_get_by_name (GST_BIN (pipeline), "decoder");
  g_signal_connect (demux, "pad-added", G_CALLBACK (link_truehd), decoder);
  source = gst_bin_get_by_name (GST_BIN (pipeline), "input");
  g_object_set (source, "location", argv[2], NULL);
  gst_object_unref (source);
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "output");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, capture_burst, &capture, NULL);
  bus = gst_element_get_bus (pipeline);

  for (round = 0; round < 3; round++) {
    capture.bursts = 0;
    g_checksum_reset (capture.checksum);
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=,
        GST_STATE_CHANGE_FAILURE);
    message = gst_bus_timed_pop_filtered (bus, 15 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    g_assert_nonnull (message);
    if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
      gchar *debug = NULL;
      gst_message_parse_error (message, &error, &debug);
      g_printerr ("%s: %s\n", error->message, debug ? debug : "");
      g_free (debug);
      g_assert_no_error (error);
    }
    gst_message_unref (message);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);
    g_assert_cmpuint (capture.bursts, >, 0);
    if (!expected) {
      expected = g_strdup (g_checksum_get_string (capture.checksum));
      expected_bursts = capture.bursts;
    }
    g_assert_cmpstr (g_checksum_get_string (capture.checksum), ==, expected);
    g_assert_cmpuint (capture.bursts, ==, expected_bursts);
    g_print ("ok - replay %u: %u complete MAT bursts, %s\n",
        round + 1, capture.bursts, expected);
  }
  g_free (expected);
  g_checksum_free (capture.checksum);
  gst_object_unref (bus);
  gst_object_unref (pad);
  gst_object_unref (sink);
  gst_object_unref (demux);
  gst_object_unref (decoder);
  gst_object_unref (pipeline);
  return 0;
}