/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdarg.h>
#include <gst/check/gstharness.h>

/* Exercise the shipping decoder, including its private FFmpeg muxer and
 * GstAudioDecoder frame queue. Only the LG hardware operations are replaced. */
#include "../../gstdtsdec.c"

static unsigned arms, disarms, resyncs;
static unsigned timing_delta;

void
lgroute_init (LgRoute *route, GstElement *element)
{
  memset (route, 0, sizeof (*route));
  route->element = element;
}

gboolean
lgroute_arm (LgRoute *route)
{
  (void) route;
  arms++;
  return TRUE;
}

void
lgroute_disarm (LgRoute *route)
{
  disarms++;
  route->configured = FALSE;
}

GstCaps *
lgroute_carrier_caps (void)
{
  return gst_caps_from_string ("audio/x-raw,format=S16LE,layout=interleaved,"
      "rate=192000,channels=8,channel-mask=(bitmask)0xc3f,acmod=(uint)0,"
      "es=(boolean)true,codec-str=(string)mat,clock-x4=(boolean)true,"
      "data-rate=(int)192000,data-channels=(int)8,data-bps=(int)16");
}

void
lgroute_log (const gchar *format, ...)
{
  if (g_str_has_prefix (format, "truehd: MAT resync")) {
    va_list args;
    va_start (args, format);
    timing_delta = va_arg (args, unsigned);
    va_end (args);
    resyncs++;
  }
}

int lgroute_control_receiver (void) { return 1; }
int lgroute_core_output (char mode[64])
{
  g_strlcpy (mode, "passThrough", 64);
  return 1;
}

typedef struct {
  const guint8 *data;
  guint size;
} Frame;

static GstHarness *
new_decoder (void)
{
  GstElement *element = g_object_new (gst_truehd_mat_get_type (),
      "passthrough", GST_DTSDEC_PASSTHROUGH_ALWAYS, NULL);
  GstHarness *harness = gst_harness_new_with_element (element, "sink", "src");
  gst_object_unref (element);
  gst_harness_set_src_caps_str (harness, "audio/x-true-hd,rate=(int)48000");
  return harness;
}

static void
feed (GstHarness *harness, const Frame *frame, guint index, guint16 shift)
{
  GstDtsDec *decoder = GST_DTSDEC (harness->element);
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, frame->size, NULL);
  GstMapInfo map;
  assert (gst_buffer_map (buffer, &map, GST_MAP_WRITE));
  memcpy (map.data, frame->data, frame->size);
  /* The muxer consumes the access unit timing word. No decoder parses the
   * rest of this synthetic payload and the same changed bytes are used in
   * the independent fresh-decoder comparison. */
  GST_WRITE_UINT16_BE (map.data + 2, GST_READ_UINT16_BE (frame->data + 2) + shift);
  gst_buffer_unmap (buffer, &map);
  GST_BUFFER_PTS (buffer) = gst_util_uint64_scale (index, GST_SECOND, 1200);
  assert (gst_harness_push (harness, buffer) == GST_FLOW_OK);
  /* Skip the device receiver startup mute. The resync must not trigger it
   * again, which the first post-reset burst comparison would detect. */
  if (!decoder->hd_route.configured) {
    decoder->hd_route.configured = TRUE;
    decoder->hd_silence = 96000;
  }
}

static GstBuffer *
pull_burst (GstHarness *harness)
{
  GstBuffer *buffer = gst_harness_try_pull (harness);
  assert (buffer && gst_buffer_get_size (buffer) == 61440);
  assert (GST_BUFFER_FLAG_IS_SET (buffer, LGAO_BUFFER_FLAG_ES));
  return buffer;
}

static void
assert_equal_bursts (GstHarness *actual, GstHarness *expected)
{
  guint count = gst_harness_buffers_in_queue (expected);
  assert (count > 0 && gst_harness_buffers_in_queue (actual) == count);
  while (count--) {
    GstBuffer *left = pull_burst (actual), *right = pull_burst (expected);
    GstMapInfo reference;
    assert (gst_buffer_map (right, &reference, GST_MAP_READ));
    assert (gst_buffer_memcmp (left, 0, reference.data, reference.size) == 0);
    assert (GST_BUFFER_PTS (left) == GST_BUFFER_PTS (right));
    gst_buffer_unmap (right, &reference);
    gst_buffer_unref (left);
    gst_buffer_unref (right);
  }
}

static void
check_flush (const Frame *frames, guint count, guint boundary)
{
  GstHarness *actual = new_decoder (), *fresh = new_decoder ();
  GstSegment segment;

  for (guint i = 0; i < boundary; i++)
    feed (actual, &frames[i], i, 0);
  while (gst_harness_buffers_in_queue (actual))
    gst_buffer_unref (pull_burst (actual));
  unsigned old_resyncs = resyncs;
  assert (gst_harness_push_event (actual, gst_event_new_flush_start ()));
  assert (gst_harness_push_event (actual, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&segment, GST_FORMAT_TIME);
  assert (gst_harness_push_event (actual, gst_event_new_segment (&segment)));
  /* The first post-seek access unit is not a major sync. Drop it while
   * waiting for the following major sync, as the fresh decoder does. */
  feed (actual, &frames[boundary - 1], boundary - 1, 1730);
  feed (fresh, &frames[boundary - 1], boundary - 1, 1730);
  /* Isolate frame accounting from receiver startup silence after a seek. */
  GST_DTSDEC (actual->element)->hd_silence = 96000;
  for (guint i = boundary; i < count; i++) {
    feed (actual, &frames[i], i, 1730);
    feed (fresh, &frames[i], i, 1730);
  }
  assert (resyncs == old_resyncs);
  assert_equal_bursts (actual, fresh);
  gst_harness_teardown (actual);
  gst_harness_teardown (fresh);
  g_print ("ok - flushing discards the old timing and waits for a new major sync\n");
}

static void
check_fallback (const Frame *frames, guint boundary)
{
  static const guint8 short_header[] = {
    0, 4, 0, 0, 0xf8, 0x72, 0x6f, 0xba,
  };
  static const guint8 unsupported_rate[] = {
    0, 5, 0, 0, 0xf8, 0x72, 0x6f, 0xba, 0x80, 0,
  };
  const Frame unsupported[] = {
    { short_header, sizeof short_header },
    { unsupported_rate, sizeof unsupported_rate },
  };

  for (guint j = 0; j < G_N_ELEMENTS (unsupported); j++) {
    GstHarness *harness = new_decoder ();
    for (guint i = 0; i < boundary; i++)
      feed (harness, &frames[i], i, 0);
    while (gst_harness_buffers_in_queue (harness))
      gst_buffer_unref (pull_burst (harness));
    unsigned old_disarms = disarms, old_resyncs = resyncs;
    feed (harness, &unsupported[j], boundary, 4250);
    assert (disarms == old_disarms + 1 && resyncs == old_resyncs);
    assert (GST_DTSDEC (harness->element)->hd_route.failed);
    assert (gst_harness_buffers_in_queue (harness) == 0);
    gst_harness_teardown (harness);
  }
  g_print ("ok - short and unsupported-rate major syncs retain the decoder fallback\n");
}

int
main (int argc, char **argv)
{
  const guint boundary = 64, count = 160;
  gchar *data;
  gsize length, offset = 0;
  Frame frames[count];

  gst_init (&argc, &argv);
  GST_DEBUG_CATEGORY_INIT (dtsdec_debug, "dtsdec", 0, "TrueHD MAT regression");
  assert (argc == 2 && g_file_get_contents (argv[1], &data, &length, NULL));
  for (guint i = 0; i < count; i++) {
    assert (length - offset >= 10);
    frames[i].data = (const guint8 *) data + offset;
    frames[i].size = (GST_READ_UINT16_BE (frames[i].data) & 0xfff) * 2;
    assert (frames[i].size >= 10 && frames[i].size <= length - offset);
    offset += frames[i].size;
  }
  assert (GST_READ_UINT16_BE (frames[0].data + 2) == 65496);
  assert (GST_READ_UINT16_BE (frames[1].data + 2) == 0);
  assert (GST_READ_UINT32_BE (frames[boundary].data + 4) == 0xf8726fba);

  for (guint gap = 0; gap <= 40; gap += 40) {
    GstHarness *harness = new_decoder ();
    /* Put the nominal wrap on a major sync rather than on the fixture's
     * original minor unit, so it exercises the same decision as the jump. */
    guint16 wrap_shift = 0 - GST_READ_UINT16_BE (frames[boundary].data + 2);
    for (guint i = 0; i < count; i++) {
      guint16 shift = wrap_shift;
      if (gap)
        shift = i < boundary ? 0 : gap;
      feed (harness, &frames[i], i, shift);
    }
    assert (resyncs == 0 && gst_harness_buffers_in_queue (harness) >= 5);
    gst_harness_teardown (harness);
  }
  g_print ("ok - ordinary 16-bit wrap and a short major-sync gap keep the muxer\n");

  GstHarness *actual = new_decoder (), *fresh = new_decoder ();
  for (guint i = 0; i < boundary; i++)
    feed (actual, &frames[i], i, 0);
  assert (gst_harness_buffers_in_queue (actual) == 2);
  while (gst_harness_buffers_in_queue (actual))
    gst_buffer_unref (pull_burst (actual));
  unsigned old_arms = arms, old_disarms = disarms;
  for (guint i = boundary; i < count; i++)
    feed (actual, &frames[i], i, 1770 - 40);
  assert (arms == old_arms && disarms == old_disarms);
  for (guint i = boundary; i < count; i++)
    feed (fresh, &frames[i], i, 1770 - 40);
  assert_equal_bursts (actual, fresh);
  assert (resyncs == 1 && timing_delta == 1770);
  assert (gst_harness_push_event (actual, gst_event_new_eos ()));
  assert (gst_harness_buffers_in_queue (actual) == 0);
  gst_harness_teardown (actual);
  gst_harness_teardown (fresh);
  g_print ("ok - 1770-sample jump resumes with byte-identical MAT and valid timestamps, without a route restart\n");
  check_flush (frames, count, boundary);
  check_fallback (frames, boundary);
  g_free (data);
  return 0;
}
