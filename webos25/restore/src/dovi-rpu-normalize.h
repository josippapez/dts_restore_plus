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
  guint8 *native;
  gsize native_capacity;
  guint16 active_area[4];
  GByteArray *output;
  gboolean failed;
  gboolean parser_initialized;
  guint frames;
} DoviNormalize;

/* The failure marker shares the stream state's lifetime without keeping the
 * native parser allocation alive after a failure. */
static guint8 dovi_disabled_state;

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
    GST_DEBUG ("P7 native RPU event=%d error=%d dm_size=%d", event, error, dm_size);
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
        GST_DEBUG ("P7 native L5 is stored as a raw block");
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
    GST_DEBUG ("P7 native writer result=%d length=%d", result, length);
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
  g_free (s->native);
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
  /* The compatibility gate fingerprints this exact library on the TV. */
  s->library = dlopen ("/usr/lib/librpu_m.so", RTLD_NOW | RTLD_LOCAL);
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

/* Find the next Annex B start code without mapping (and possibly merging)
 * the whole buffer. A start code can span two separate GstMemory blocks. */
static gboolean
dovi_annex_b_end (GstBuffer *input, gsize offset, gsize size, gsize *end)
{
  guint first, count, i;
  gsize skip, pos = offset;

  *end = size;
  if (size - offset < 3)
    return TRUE;
  if (!gst_buffer_find_memory (input, offset, size - offset, &first, &count,
          &skip))
    return FALSE;
  for (i = first; i < first + count; i++) {
    GstMapInfo map;
    const guint8 *candidate, *scan;
    gsize left;

    if (!gst_memory_map (gst_buffer_peek_memory (input, i), &map, GST_MAP_READ))
      return FALSE;
    scan = map.data + skip;
    left = map.size - skip;
    while (left && (candidate = memchr (scan, 1, left))) {
      gsize at = pos + (candidate - map.data) - skip;
      gsize back = MIN (at - offset, 3);

      if (back >= 2) {
        guint8 split[3];
        const guint8 *preceding;

        if ((gsize) (candidate - map.data) >= back) {
          preceding = candidate - back;
        } else {
          if (gst_buffer_extract (input, at - back, split, back) != back) {
            gst_memory_unmap (map.memory, &map);
            return FALSE;
          }
          preceding = split;
        }
        if (preceding[back - 2] == 0 && preceding[back - 1] == 0) {
          *end = at - ((back == 3 && preceding[0] == 0) ? 3 : 2);
          gst_memory_unmap (map.memory, &map);
          return TRUE;
        }
      }
      left -= candidate - scan + 1;
      scan = candidate + 1;
    }
    pos += map.size - skip;
    skip = 0;
    gst_memory_unmap (map.memory, &map);
  }
  return TRUE;
}

static gboolean
dovi_rewrite_nal (DoviNormalize *s, GstBuffer *input, gsize nal,
    gboolean byte_stream, gsize *payload_end)
{
  gsize size = *payload_end - nal;
  gsize native_size = size + 2;
  gint status;

  if (s->native_capacity < native_size) {
    guint8 *native = g_try_realloc (s->native, native_size);
    if (!native)
      return FALSE;
    s->native = native;
    s->native_capacity = native_size;
  }
  memcpy (s->native, "\0\0\0\1", 4);
  if (gst_buffer_extract (input, nal + 2, s->native + 4, size - 2) != size - 2)
    return FALSE;
  /* Annex B allows trailing_zero_8bits before a new start code or EOF.
   * They are outside the RPU and remain in the original buffer span. */
  if (byte_stream) {
    while (native_size > 4 && s->native[native_size - 1] == 0)
      native_size--;
    if (native_size == 4)
      return FALSE;
    *payload_end = nal + native_size - 2;
  }
  g_byte_array_set_size (s->output, 0);
  s->failed = FALSE;
  status = s->process (s->parser, 0, NULL, s->native, native_size, 1);
  if (status != 0 || s->failed || !s->output->len) {
    GST_DEBUG ("P7 RPU status=%d callback_failure=%d output=%u",
        status, s->failed, s->output->len);
    return FALSE;
  }
  return TRUE;
}

/* Process the output access unit before encoded layer assembly, if any.
 * Share untouched memory and preserve timing, flags and metadata. The original
 * AU remains available if any RPU in it fails. */
static GstBuffer *
dovi_normalize_buffer (GstElement *element, GstBuffer *input,
    gboolean byte_stream, guint length_size)
{
  gpointer state = g_object_get_qdata (G_OBJECT (element), dovi_state_key ());
  DoviNormalize *s = state;
  GstBuffer *result = NULL;
  gsize offset = 0, last = 0, size;
  const gchar *failure = "invalid NAL framing";

  if (state == &dovi_disabled_state)
    return gst_buffer_ref (input);
  size = gst_buffer_get_size (input);
  while (offset < size) {
    guint8 header[6];
    gsize nal, end, available = MIN (size - offset, sizeof header);
    guint prefix = length_size;

    if (gst_buffer_extract (input, offset, header, available) != available)
      goto fail;
    if (byte_stream) {
      if (available >= 4 && !memcmp (header, "\0\0\0\1", 4))
        prefix = 4;
      else if (available >= 3 && !memcmp (header, "\0\0\1", 3))
        prefix = 3;
      else
        goto fail;
      nal = offset + prefix;
      if (!dovi_annex_b_end (input, nal, size, &end))
        goto fail;
    } else {
      guint nal_size = 0, i;

      if (prefix < 1 || prefix > 4 || available < prefix)
        goto fail;
      for (i = 0; i < prefix; i++)
        nal_size = (nal_size << 8) | header[i];
      nal = offset + prefix;
      if (nal_size > size - nal)
        goto fail;
      end = nal + nal_size;
    }
    if (end - nal < 2 || available < prefix + 2)
      goto fail;
    if (header[prefix] == 0x7c && header[prefix + 1] == 1) {
      guint8 *replacement;
      gsize replacement_size, payload_end = end;
      guint i;

      if (end - nal < 3 || end - nal > 65536)
        goto fail;
      failure = "native RPU initialization or rewrite failed";
      if (!s) {
        s = dovi_normalize_create ();
        if (!s)
          goto fail;
        g_object_set_qdata_full (G_OBJECT (element), dovi_state_key (), s,
            dovi_normalize_free);
        GST_INFO_OBJECT (element, "P7 BL-only RPU normalization initialized");
      }
      if (!dovi_rewrite_nal (s, input, nal, byte_stream, &payload_end))
        goto fail;

      failure = "could not assemble the rewritten AU";
      if (!byte_stream && prefix < 4 && s->output->len >= (1u << (8 * prefix)))
        goto fail;
      if (!result)
        result = gst_buffer_new ();
      if (offset > last && !gst_buffer_copy_into (result, input,
              GST_BUFFER_COPY_MEMORY, last, offset - last))
        goto fail;
      replacement_size = prefix + s->output->len;
      replacement = g_try_malloc (replacement_size);
      if (!replacement)
        goto fail;
      if (byte_stream) {
        memcpy (replacement, header, prefix);
      } else {
        for (i = 0; i < prefix; i++)
          replacement[prefix - 1 - i] = s->output->len >> (8 * i);
      }
      memcpy (replacement + prefix, s->output->data, s->output->len);
      gst_buffer_append_memory (result, gst_memory_new_wrapped (0, replacement,
              replacement_size, 0, replacement_size, replacement, g_free));
      last = payload_end;
      failure = "invalid NAL framing";
    }
    offset = end;
  }
  if (!result)
    return gst_buffer_ref (input);
  failure = "could not restore AU data or metadata";
  if ((size > last && !gst_buffer_copy_into (result, input,
              GST_BUFFER_COPY_MEMORY, last, size - last)) ||
      !gst_buffer_copy_into (result, input, GST_BUFFER_COPY_METADATA, 0, -1))
    goto fail;
  return result;

fail:
  GST_WARNING_OBJECT (element, "P7 RPU normalization disabled for this stream: "
      "%s at byte %" G_GSIZE_FORMAT " after %u rewritten RPUs; passing original AU",
      failure, offset, s ? s->frames : 0);
  gst_clear_buffer (&result);
  g_object_set_qdata_full (G_OBJECT (element), dovi_state_key (),
      &dovi_disabled_state, NULL);
  return gst_buffer_ref (input);
}
