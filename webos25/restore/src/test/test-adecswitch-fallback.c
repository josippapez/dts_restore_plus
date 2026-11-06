#include "../gstadecswitch.c"

static guint hd_frames, pcm_frames;
static _Thread_local gboolean in_chain;

typedef struct { GstElement parent; GstPad *sink, *src; } TestDecoder;
typedef struct { GstElementClass parent; } TestDecoderClass;
G_DEFINE_TYPE (TestDecoder, test_decoder, GST_TYPE_ELEMENT)

static GstFlowReturn
frame (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GstElement *element = GST_ELEMENT (parent);
  GstObject *owner = gst_object_get_parent (parent);
  GstAdecSwitch *self = GST_ADECSWITCH (owner);
  (void) pad;
  g_assert_true (self->decoder == element);
  in_chain = TRUE;
  if (!strcmp (self->decoder_factory, "truehdmat")) {
    hd_frames++;
    gst_element_post_message (element, gst_message_new_element (GST_OBJECT (element),
        gst_structure_new_empty ("truehdmat-fallback")));
    g_assert_true (self->decoder == element);
  } else {
    pcm_frames++;
  }
  in_chain = FALSE;
  gst_object_unref (owner);
  gst_buffer_unref (buffer);
  return GST_FLOW_OK;
}

static GstStateChangeReturn
state (GstElement *element, GstStateChange transition)
{
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
    g_assert_false (in_chain);
  return GST_ELEMENT_CLASS (test_decoder_parent_class)->change_state (element, transition);
}

static void
test_decoder_class_init (TestDecoderClass *klass)
{
  GstElementClass *element = GST_ELEMENT_CLASS (klass);
  GstCaps *caps = gst_caps_from_string ("audio/x-true-hd");
  gst_element_class_add_pad_template (element, gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);
  caps = gst_caps_from_string ("audio/x-raw");
  gst_element_class_add_pad_template (element, gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);
  gst_element_class_set_static_metadata (element, "Test decoder", "Codec/Decoder/Audio", "Fallback test", "test");
  element->change_state = state;
}

static void
test_decoder_init (TestDecoder *self)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (self);
  self->sink = gst_pad_new_from_template (gst_element_class_get_pad_template (klass, "sink"), "sink");
  self->src = gst_pad_new_from_template (gst_element_class_get_pad_template (klass, "src"), "src");
  gst_pad_set_chain_function (self->sink, frame);
  gst_element_add_pad (GST_ELEMENT (self), self->sink);
  gst_element_add_pad (GST_ELEMENT (self), self->src);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_assert_true (gst_element_register (NULL, "truehdmat", GST_RANK_NONE, test_decoder_get_type ()));
  g_assert_true (gst_element_register (NULL, "avdec_truehd", GST_RANK_NONE, test_decoder_get_type ()));
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstAdecSwitch *self = g_object_new (GST_TYPE_ADECSWITCH, NULL);
  gst_bin_add (GST_BIN (pipeline), GST_ELEMENT (self));
  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  GstPad *upstream = gst_pad_new ("test-source", GST_PAD_SRC);
  g_assert_true (gst_pad_set_active (upstream, TRUE));
  g_assert_cmpint (gst_pad_link (upstream, self->sinkpad), ==, GST_PAD_LINK_OK);
  g_assert_true (gst_pad_push_event (upstream, gst_event_new_stream_start ("test")));
  GstCaps *caps = gst_caps_from_string ("audio/x-true-hd,rate=48000,channels=6");
  g_assert_true (gst_pad_push_event (upstream, gst_event_new_caps (caps)));
  g_assert_cmpint (gst_adecswitch_plug (self, "truehdmat", caps), ==, GST_PAD_PROBE_OK);
  GstSegment segment;
  gst_segment_init (&segment, GST_FORMAT_TIME);
  g_assert_true (gst_pad_push_event (upstream, gst_event_new_segment (&segment)));
  g_assert_cmpint (gst_pad_push (upstream, gst_buffer_new_allocate (NULL, 10, NULL)), ==, GST_FLOW_OK);
  g_assert_cmpuint (hd_frames, ==, 1);
  g_assert_true (self->truehd_fallback);
  g_assert_cmpint (gst_pad_push (upstream, gst_buffer_new_allocate (NULL, 10, NULL)), ==, GST_FLOW_OK);
  g_assert_cmpuint (pcm_frames, ==, 1);
  g_assert_false (self->truehd_fallback);
  g_assert_cmpstr (self->decoder_factory, ==, "avdec_truehd");
  gst_pad_unlink (upstream, self->sinkpad);
  gst_pad_set_active (upstream, FALSE);
  gst_object_unref (upstream);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  gst_caps_unref (caps);
  g_print ("TrueHD fallback switches on the next input, without in-chain teardown\n");
  return 0;
}
