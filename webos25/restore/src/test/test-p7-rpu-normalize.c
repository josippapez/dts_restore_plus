/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Run on the TV with one supplied P7 FEL HEVC RPU NAL (7c 01 19 ...).
 * Exercises the production implementation against LG's native CRC parser.
 */
#include <gst/gst.h>
#include <stdio.h>
GST_DEBUG_CATEGORY_STATIC (test_debug);
#define GST_CAT_DEFAULT test_debug
#include "../dovi-rpu-normalize.h"

typedef struct {
  guint8 header[316], dm[67396];
  gpointer payload;
  gint calls;
} Parsed;

static void
read_callback (gpointer ctx, gint event, gpointer sequence, guint8 *header,
    gpointer payload, gint size, guint8 *dm, gint dm_size, gint error)
{
  Parsed *p = ctx;
  g_assert_cmpint (event, ==, 0);
  g_assert_cmpint (dm_size, ==, sizeof p->dm);
  memcpy (p->header, header, sizeof p->header);
  memcpy (p->dm, dm, sizeof p->dm);
  p->payload = payload;
  p->calls++;
}

static gpointer
reader (DoviNormalize *api, Parsed *parsed)
{
  gint (*init) (gpointer, gpointer) = dlsym (api->library, "dv_md_parser_init");
  DoviNativeConf conf = { 0 };
  gpointer handle = g_malloc0 (17354416);
  conf.callback = read_callback;
  conf.context = parsed;
  conf.flags[0] = 1;
  conf.flags[1] = 2;
  conf.flags[5] = 1;
  conf.flags[6] = 1;
  g_assert_cmpint (init (handle, &conf), ==, 0);
  return handle;
}

static void
read_nal (DoviNormalize *api, gpointer handle, const guint8 *nal, gsize size)
{
  GByteArray *native;

  g_assert_cmpuint (size, >=, 3);
  g_assert_cmpuint (size, <=, 65536);
  g_assert_cmpmem (nal, 2, "\x7c\x01", 2);
  native = g_byte_array_new ();
  g_byte_array_append (native, (const guint8 *) "\0\0\0\1", 4);
  g_byte_array_append (native, nal + 2, size - 2);
  g_assert_cmpint (api->process (handle, 0, NULL, native->data, native->len, 1), ==, 0);
  g_byte_array_unref (native);
}

static GBytes *
fixture (DoviNormalize *api, Parsed *original, gint area)
{
  Parsed p = *original;
  gpointer sample[3], bytes = NULL;
  gint size, i, count = 0;
  GByteArray *nal;
  /* Generate absent, nonzero and explicit-zero L5 using the stock writer. */
  for (i = 0; i < p.dm[74]; i++) {
    guint8 *block = p.dm + 76 + 264 * i;
    if (block[4] != 5) {
      memmove (p.dm + 76 + 264 * count, block, 264);
      count++;
    }
  }
  if (area >= 0) {
    guint8 *block;
    g_assert_cmpint (count, <, G_MAXUINT8);
    block = p.dm + 76 + 264 * count++;
    memset (block, 0, 264);
    GST_WRITE_UINT32_LE (block, 7);
    block[4] = 5;
    GST_WRITE_UINT16_LE (block + 12, area);
    GST_WRITE_UINT16_LE (block + 14, area);
  }
  p.dm[74] = count;
  p.header[6] = 1;
  p.header[26] = 0;
  sample[0] = p.header; sample[1] = p.payload; sample[2] = p.dm;
  g_assert_cmpint (api->write (api->writer, sample, &bytes, &size), >=, 0);
  g_assert_cmpint (size, >, 5);
  nal = g_byte_array_new ();
  g_byte_array_append (nal, (const guint8 *) "\x7c\x01", 2);
  g_byte_array_append (nal, (guint8 *) bytes + 4, size - 4);
  return g_byte_array_free_to_bytes (nal);
}

static void
check (DoviNormalize *api, Parsed *original, GstElement *element,
    GBytes *nal, gboolean annex_b, gint area)
{
  gsize size;
  const guint8 *data = g_bytes_get_data (nal, &size);
  GstBuffer *input = gst_buffer_new_allocate (NULL, size + 4, NULL), *output;
  GstMapInfo map;
  Parsed p = { 0 };
  gpointer handle = reader (api, &p);
  guint8 prefix[4] = { 0, 0, 0, 1 };
  gboolean found = FALSE;
  guint i;
  if (!annex_b) GST_WRITE_UINT32_BE (prefix, size);
  gst_buffer_fill (input, 0, prefix, 4);
  gst_buffer_fill (input, 4, data, size);
  GST_BUFFER_PTS (input) = 123456;
  GST_BUFFER_DURATION (input) = 41708333;
  GST_BUFFER_FLAG_SET (input, GST_BUFFER_FLAG_DISCONT);
  output = dovi_normalize_buffer (element, input, annex_b, 4);
  g_assert_nonnull (output);
  g_assert_cmpuint (GST_BUFFER_PTS (output), ==, GST_BUFFER_PTS (input));
  g_assert_cmpuint (GST_BUFFER_DURATION (output), ==, GST_BUFFER_DURATION (input));
  g_assert_true (GST_BUFFER_FLAG_IS_SET (output, GST_BUFFER_FLAG_DISCONT));
  gst_buffer_map (output, &map, GST_MAP_READ);
  if (!annex_b) g_assert_cmpuint (GST_READ_UINT32_BE (map.data), ==, map.size - 4);
  read_nal (api, handle, map.data + 4, map.size - 4);
  g_assert_cmpint (p.calls, ==, 1);
  g_assert_cmpint (p.header[22], ==, 1);
  /* Color matrices, signal range, mastering luminance and scene metadata. */
  g_assert_cmpmem (p.dm, 74, original->dm, 74);
  for (i = 0; i < p.dm[74]; i++) {
    guint8 *block = p.dm + 76 + 264 * i;
    if (block[4] == 5) {
      g_assert_cmpint (block[5], ==, 0);
      g_assert_cmpuint (GST_READ_UINT16_LE (block + 8), ==, 0);
      g_assert_cmpuint (GST_READ_UINT16_LE (block + 10), ==, 0);
      g_assert_cmpuint (GST_READ_UINT16_LE (block + 12), ==, area);
      g_assert_cmpuint (GST_READ_UINT16_LE (block + 14), ==, area);
      found = TRUE;
    }
  }
  g_assert_true (found);
  api->destroy (handle); g_free (handle);
  gst_buffer_unmap (output, &map);
  gst_buffer_unref (output); gst_buffer_unref (input);
}

int main (int argc, char **argv)
{
  DoviNormalize *api;
  Parsed original = { 0 };
  gpointer handle;
  gchar *data;
  gsize size;
  GBytes *absent, *bars, *zero;
  GstElement *element;
  gst_init (&argc, &argv);
  GST_DEBUG_CATEGORY_INIT (test_debug, "p7-rpu-test", 0, "P7 RPU test");
  /* The assembly marker does not change which profile needs normalization. */
  {
    gint profile, compositor;
    for (profile = 4; profile <= 8; profile++) {
      for (compositor = 0; compositor <= 1; compositor++) {
        GstStructure *caps = gst_structure_new ("video/x-h265",
            "dolby-vision", G_TYPE_BOOLEAN, TRUE,
            "dolby-vision-profile", G_TYPE_INT, profile,
            "need-compositor", G_TYPE_BOOLEAN, compositor, NULL);
        g_assert_cmpint (dovi_bl_rpu_required (caps), ==, profile == 7);
        gst_structure_set (caps, "dolby-vision", G_TYPE_BOOLEAN, FALSE, NULL);
        g_assert_false (dovi_bl_rpu_required (caps));
        gst_structure_free (caps);
      }
    }
  }
  if (argc != 2) {
    g_printerr ("Usage: %s FEL_HEVC_RPU_NAL\n", argv[0]);
    return 2;
  }
  g_assert_true (g_file_get_contents (argv[1], &data, &size, NULL));
  api = dovi_normalize_create ();
  g_assert_nonnull (api);
  handle = reader (api, &original);
  read_nal (api, handle, (guint8 *) data, size);
  g_assert_cmpint (original.calls, ==, 1);
  g_assert_cmpint (original.header[22], ==, 0); /* actually FEL */
  absent = fixture (api, &original, -1);
  bars = fixture (api, &original, 140);
  zero = fixture (api, &original, 0);
  element = gst_bin_new ("stream-one");
  check (api, &original, element, absent, TRUE, 0);
  check (api, &original, element, bars, TRUE, 140);
  check (api, &original, element, absent, TRUE, 140);
  check (api, &original, element, zero, TRUE, 0);
  check (api, &original, element, absent, FALSE, 0);
  gst_object_unref (element);
  element = gst_bin_new ("stream-two");
  check (api, &original, element, bars, FALSE, 140);
  /* Same parser object, new STREAM_START: per-stream state must be destroyed. */
  g_object_set_qdata_full (G_OBJECT (element), dovi_state_key (), NULL, NULL);
  check (api, &original, element, absent, FALSE, 0);
  gst_object_unref (element);
  api->destroy (handle); g_free (handle);
  dovi_normalize_free (api);
  g_bytes_unref (absent); g_bytes_unref (bars); g_bytes_unref (zero); g_free (data);
  puts ("PASS: P7 profile selection, residual flag, native CRC, active area and buffer metadata");
  return 0;
}
