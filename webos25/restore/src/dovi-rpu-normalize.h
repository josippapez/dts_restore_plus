/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Profile 7 compatibility playback decodes only the base layer. Rewrite the
 * RPU so the Dolby backend can commit the stream's own color configuration.
 * The TV's native reader and writer resolve mapping references, escape the
 * payload and regenerate its CRC. The native layouts below match the known
 * librpu_m ABI, guarded by the activation and runtime compatibility checks.
 */
#include <dlfcn.h>
#include <string.h>

typedef struct {
  gpointer callback, context;
  gint flags[7]; /* profile, level, CRC, payload, RBSP, MEL, changing sequence */
  gpointer log;
} DoviNativeConf;

typedef struct {
  gpointer library, parser, writer, writer_sample;
  gint (*process) (gpointer, gint, gpointer, gpointer, gint, gint);
  gint (*write) (gpointer, gpointer, gpointer *, gint *);
  void (*destroy) (gpointer);
  void (*writer_cleanup) (gpointer, gpointer);
  guint8 *dm;
  guint16 active_area[4];
  GByteArray *output;
  gboolean failed;
  gboolean parser_initialized;
  guint frames;
} DoviNormalize;

static GQuark
dovi_state_key (void)
{
  return g_quark_from_static_string ("dts-restore-p7-bl-rpu-state");
}

static gboolean
dovi_bl_rpu_required (const GstStructure *caps)
{
  gint profile = 0;
  gboolean dovi = FALSE;
  gst_structure_get_int (caps, "dolby-vision-profile", &profile);
  gst_structure_get_boolean (caps, "dolby-vision", &dovi);
  return dovi && profile == 7;
}

static void
dovi_native_callback (gpointer opaque, gint event, gpointer sequence,
    guint8 *header, gpointer payload, gint payload_size, guint8 *dm,
    gint dm_size, gint error)
{
  DoviNormalize *s = opaque;
  guint8 normalized_header[316];
  gpointer sample[3], bytes = NULL;
  gint length = 0, result;
  guint i, count;
  gboolean has_area = FALSE;

  if (event != 0 || !header || !payload || !dm || dm_size != 67396) {
    GST_WARNING ("P7 native RPU event=%d error=%d dm_size=%d", event, error, dm_size);
    s->failed = TRUE;
    return;
  }
  memcpy (normalized_header, header, sizeof normalized_header);
  /* The current decoder supplies BL only. Publish a complete sequence/mapping
   * even when the input references a previous RPU, with no EL residual demand.
   * The native writer removes the now-inapplicable NLQ syntax and fixes CRC. */
  normalized_header[6] = 1; /* vdr_seq_info_present_flag */
  normalized_header[22] = 1; /* sequence.disable_residual_flag */
  normalized_header[26] = 0; /* use_prev_vdr_rpu_flag: payload is resolved */
  memcpy (s->dm, dm, dm_size);
  count = s->dm[74];
  for (i = 0; i < count; i++) {
    guint8 *block = s->dm + 76 + 264 * i;
    if (block[4] == 5) {
      if (block[5]) { /* raw/unknown L5 is not the expected native ABI */
        GST_WARNING ("P7 native L5 is stored as a raw block");
        s->failed = TRUE;
        return;
      }
      memcpy (s->active_area, block + 8, sizeof s->active_area);
      has_area = TRUE;
    }
  }
  /* Missing L5 retains this stream's last area, initially the full frame.
   * Explicit zero clears the area, and explicit nonzero sets new offsets. */
  if (!has_area) {
    guint8 *block;
    if (count == 255) {
      s->failed = TRUE;
      return;
    }
    block = s->dm + 76 + 264 * count;
    memset (block, 0, 264);
    GST_WRITE_UINT32_LE (block, 7); /* four 13-bit offsets, padded to bytes */
    block[4] = 5;
    memcpy (block + 8, s->active_area, sizeof s->active_area);
    s->dm[74] = count + 1;
  }
  sample[0] = normalized_header;
  sample[1] = payload;
  sample[2] = s->dm;
  result = s->write (s->writer, sample, &bytes, &length);
  /* Writer emits Annex B AVC-style RPU header 00 00 00 01 19. HEVC carries
   * that same RPU payload behind 7c 01 (UNSPEC62). */
  if (result < 0 || length < 6 || length > 65536 || !bytes ||
      memcmp (bytes, "\0\0\0\1\x19", 5)) {
    GST_WARNING ("P7 native writer result=%d length=%d", result, length);
    s->failed = TRUE;
    return;
  }
  g_byte_array_append (s->output, (const guint8 *) "\x7c\x01", 2);
  g_byte_array_append (s->output, (guint8 *) bytes + 4, length - 4);
  s->frames++;
}

static void
dovi_normalize_free (gpointer opaque)
{
  DoviNormalize *s = opaque;
  if (!s) return;
  if (s->parser_initialized && s->destroy) s->destroy (s->parser);
  if (s->writer && s->writer_cleanup)
    s->writer_cleanup (s->writer, s->writer_sample);
  g_free (s->parser);
  g_free (s->dm);
  if (s->output) g_byte_array_unref (s->output);
  if (s->library) dlclose (s->library);
  g_free (s);
}

static DoviNormalize *
dovi_normalize_create (void)
{
  DoviNormalize *s = g_new0 (DoviNormalize, 1);
  DoviNativeConf conf = { 0 };
  gint (*query) (gpointer), (*init) (gpointer, gpointer);
  gint (*writer_init) (gpointer *, gpointer *, gpointer);
  gint size;
  G_STATIC_ASSERT (sizeof (DoviNativeConf) == 40);
  s->library = dlopen ("librpu_m.so", RTLD_NOW | RTLD_LOCAL);
  if (!s->library) goto fail;
#define DOVI_API(member, symbol) \
  do { *(gpointer *) &(member) = dlsym (s->library, symbol); \
    if (!(member)) goto fail; } while (0)
  DOVI_API (query, "dv_md_parser_mem_query");
  DOVI_API (init, "dv_md_parser_init");
  DOVI_API (writer_init, "rpu_writer_init");
  DOVI_API (s->process, "dv_md_parser_process");
  DOVI_API (s->write, "rpu_write");
  DOVI_API (s->destroy, "dv_md_parser_destroy");
  DOVI_API (s->writer_cleanup, "rpu_writer_cleanup");
#undef DOVI_API
  conf.callback = dovi_native_callback;
  conf.context = s;
  conf.flags[0] = 1; /* RPU_MAIN_PROFILE */
  conf.flags[1] = 2; /* RPU_LEVEL_3 */
  conf.flags[5] = 1; /* preserve parsed data; don't classify MEL */
  conf.flags[6] = 1; /* legal sequence changes within a stream */
  size = query (&conf);
  if (size != 17354416) goto fail; /* verified webOS 25 native ABI */
  s->parser = g_try_malloc0 (size);
  s->dm = g_try_malloc0 (67396);
  s->output = g_byte_array_new ();
  if (!s->parser || !s->dm || init (s->parser, &conf) != 0) goto fail;
  s->parser_initialized = TRUE;
  if (writer_init (&s->writer, &s->writer_sample, conf.flags) != 0) goto fail;
  return s;
fail:
  dovi_normalize_free (s);
  return NULL;
}

/* Process the output access unit before encoded layer assembly, if any.
 * Preserve non-RPU NAL bytes as well as the input timing, flags and metadata. */
static GstBuffer *
dovi_normalize_buffer (GstElement *element, GstBuffer *input,
    gboolean byte_stream, guint length_size)
{
  DoviNormalize *s = g_object_get_qdata (G_OBJECT (element), dovi_state_key ());
  GstMapInfo map;
  GByteArray *au = NULL;
  GstBuffer *result = NULL;
  gsize offset = 0;
  gboolean changed = FALSE;
  if (!gst_buffer_map (input, &map, GST_MAP_READ)) return NULL;
  au = g_byte_array_sized_new (map.size);
  while (offset < map.size) {
    gsize start = offset, nal, end;
    guint prefix = length_size;
    if (byte_stream) {
      if (map.size - offset >= 4 && !memcmp (map.data + offset, "\0\0\0\1", 4))
        prefix = 4;
      else if (map.size - offset >= 3 && !memcmp (map.data + offset, "\0\0\1", 3))
        prefix = 3;
      else goto fail;
      nal = offset + prefix;
      for (end = nal; end + 3 <= map.size; end++)
        if (!memcmp (map.data + end, "\0\0\1", 3) ||
            (end + 4 <= map.size && !memcmp (map.data + end, "\0\0\0\1", 4))) break;
      if (end + 3 > map.size) end = map.size;
    } else {
      guint size = 0, i;
      if (prefix < 1 || prefix > 4 || map.size - offset < prefix) goto fail;
      for (i = 0; i < prefix; i++) size = (size << 8) | map.data[offset + i];
      nal = offset + prefix;
      if (size > map.size - nal) goto fail;
      end = nal + size;
    }
    if (end - nal < 2) goto fail;
    if (((map.data[nal] >> 1) & 63) == 62 && map.data[nal] == 0x7c &&
        map.data[nal + 1] == 1) {
      GByteArray *native;
      guint i;
      gint status;
      if (end - nal > 65536) goto fail;
      if (!s) {
        s = dovi_normalize_create ();
        if (!s) goto fail;
        g_object_set_qdata_full (G_OBJECT (element), dovi_state_key (), s,
            dovi_normalize_free);
        GST_INFO_OBJECT (element, "P7 BL-only RPU normalization initialized");
      }
      g_byte_array_set_size (s->output, 0);
      s->failed = FALSE;
      native = g_byte_array_new ();
      g_byte_array_append (native, (const guint8 *) "\0\0\0\1", 4);
      g_byte_array_append (native, map.data + nal + 2, end - nal - 2);
      status = s->process (s->parser, 0, NULL, native->data, native->len, 1);
      g_byte_array_unref (native);
      if (status != 0 || s->failed || !s->output->len) {
        GST_WARNING_OBJECT (element, "P7 RPU failed after %u frames: status=%d callback_failure=%d output=%u",
            s->frames, status, s->failed, s->output->len);
        goto fail;
      }
      if (byte_stream) g_byte_array_append (au, map.data + start, prefix);
      else {
        guint8 sizes[4];
        if (prefix < 4 && s->output->len >= (1u << (8 * prefix))) goto fail;
        for (i = 0; i < prefix; i++)
          sizes[prefix - 1 - i] = s->output->len >> (8 * i);
        g_byte_array_append (au, sizes, prefix);
      }
      g_byte_array_append (au, s->output->data, s->output->len);
      changed = TRUE;
    } else g_byte_array_append (au, map.data + start, end - start);
    offset = end;
  }
  if (changed) {
    result = gst_buffer_new_allocate (NULL, au->len, NULL);
    gst_buffer_fill (result, 0, au->data, au->len);
    gst_buffer_copy_into (result, input, GST_BUFFER_COPY_METADATA, 0, -1);
  } else result = gst_buffer_ref (input);
  g_byte_array_unref (au);
  gst_buffer_unmap (input, &map);
  return result;
fail:
  GST_WARNING_OBJECT (element, "P7 AU framing failed: byte_stream=%d length_size=%u offset=%" G_GSIZE_FORMAT " size=%" G_GSIZE_FORMAT,
      byte_stream, length_size, offset, map.size);
  if (au) g_byte_array_unref (au);
  gst_buffer_unmap (input, &map);
  GST_ELEMENT_ERROR (element, STREAM, DECODE,
      ("Cannot initialize P7 base-layer Dolby metadata"),
      ("Native RPU parse/write or ABI validation failed; refusing stale Dolby state"));
  return NULL;
}
