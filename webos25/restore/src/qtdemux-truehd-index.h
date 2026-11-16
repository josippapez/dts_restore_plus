/* Coalesce contiguous TrueHD access units inside each MP4 chunk, splitting at
 * every major sync. Preserve bytes, timeline and random-access points while
 * avoiding a 32-byte index entry for every ~0.8 ms audio packet. */
static gboolean
qtdemux_compact_truehd_index (GstQTDemux * demux, QtDemuxStream * stream)
{
  GstByteReader sizes = stream->stsz, times = stream->stts;
  GstByteReader chunks = stream->stsc, offsets = stream->stco;
  GstByteReader syncs = stream->stss;
  GstByteWriter sz, ts, co, ss;
  guint32 chunk_count, chunk, sample = 0, groups = 0, sync_count = 0;
  guint32 run_left = 0, delta = 0, next_sync = 0, sync_left;
  guint32 per_chunk = 0, description = 0, next_chunk = 1, sc_left;
  gboolean ok = FALSE;

  if (demux->fragmented || stream->stsd_entries_length != 1 ||
      stream->ctts_present || stream->stps_present ||
      stream->n_samples > QTDEMUX_MAX_MOOV_SIZE / sizeof (guint32))
    return TRUE; /* Leave unsupported layouts to the existing bounded path. */

  chunk_count = GST_READ_UINT32_BE (offsets.data + 4);
  sc_left = stream->n_samples_per_chunk;
  sync_left = stream->stss_present ? stream->n_sample_syncs : 0;
  if (sync_left) {
    if (!gst_byte_reader_get_uint32_be (&syncs, &next_sync) ||
        next_sync == 0 || next_sync > stream->n_samples)
      return FALSE;
    /* Leave leading non-sync samples to the ordinary bounded index. */
    if (next_sync != 1)
      return TRUE;
    sync_left--;
  }
  gst_byte_writer_init (&sz);
  gst_byte_writer_init (&ts);
  gst_byte_writer_init (&co);
  gst_byte_writer_init (&ss);
  /* stco/co64 retains its version/flags and entry-count prefix. */
  if (!gst_byte_writer_put_uint32_be (&co, 0) ||
      !gst_byte_writer_put_uint32_be (&co, 0))
    goto done;

  for (chunk = 1; chunk <= chunk_count; chunk++) {
    guint64 offset;
    guint32 in_chunk = 0;
    if (chunk == next_chunk) {
      guint32 first;
      if (!sc_left || !gst_byte_reader_get_uint32_be (&chunks, &first) ||
          first != chunk ||
          !gst_byte_reader_get_uint32_be (&chunks, &per_chunk) || !per_chunk ||
          !gst_byte_reader_get_uint32_be (&chunks, &description) || description != 1)
        goto done;
      sc_left--;
      next_chunk = G_MAXUINT32;
      if (sc_left && (!gst_byte_reader_peek_uint32_be (&chunks, &next_chunk) ||
              next_chunk <= chunk || next_chunk > chunk_count))
        goto done;
    }
    if (stream->co_size == 8) {
      if (!gst_byte_reader_get_uint64_be (&offsets, &offset))
        goto done;
    } else {
      guint32 value;
      if (!gst_byte_reader_get_uint32_be (&offsets, &value))
        goto done;
      offset = value;
    }
    while (in_chunk < per_chunk) {
      guint64 bytes = 0, duration = 0;
      gboolean keyframe = !stream->stss_present || !stream->n_sample_syncs ||
          sample + 1 == next_sync;
      if (++groups >= QTDEMUX_MAX_SAMPLE_INDEX_SIZE / sizeof (QtDemuxSample))
        goto done;
      if (keyframe) {
        if (!gst_byte_writer_put_uint32_be (&ss, groups))
          goto done;
        sync_count++;
      }
      do {
        guint32 size = stream->sample_size;
        if (sample >= stream->n_samples ||
            (!size && !gst_byte_reader_get_uint32_be (&sizes, &size)))
          goto done;
        if (!run_left && (!gst_byte_reader_get_uint32_be (&times, &run_left) ||
                !run_left || !gst_byte_reader_get_uint32_be (&times, &delta) ||
                !delta || delta > G_MAXINT32))
          goto done;
        if (sample + 1 == next_sync) {
          guint32 previous = next_sync;
          next_sync = 0;
          if (sync_left) {
            if (!gst_byte_reader_get_uint32_be (&syncs, &next_sync) ||
                next_sync <= previous || next_sync > stream->n_samples)
              goto done;
            sync_left--;
          }
        }
        bytes += size;
        duration += delta;
        run_left--;
        sample++;
        in_chunk++;
        if (bytes > G_MAXINT32 || duration > G_MAXINT32)
          goto done;
      } while (in_chunk < per_chunk && sample + 1 != next_sync);
      if (offset > G_MAXUINT64 - bytes)
        goto done;
      if (!gst_byte_writer_put_uint32_be (&sz, bytes) ||
          !gst_byte_writer_put_uint32_be (&ts, 1) ||
          !gst_byte_writer_put_uint32_be (&ts, duration) ||
          !gst_byte_writer_put_uint64_be (&co, offset))
        goto done;
      offset += bytes;
    }
  }
  if (sample != stream->n_samples || run_left ||
      gst_byte_reader_get_remaining (&times) || sync_left || sc_left)
    goto done;

  /* All validation precedes replacement; failure leaves the input unchanged. */
#define REPLACE_READER(field, writer) G_STMT_START { \
  guint length = gst_byte_writer_get_size (&writer); \
  guint8 *data = gst_byte_writer_reset_and_get_data (&writer); \
  g_free ((gpointer) stream->field.data); \
  gst_byte_reader_init (&stream->field, data, length); \
} G_STMT_END
  if (!gst_byte_writer_set_pos (&co, 4) ||
      !gst_byte_writer_put_uint32_be (&co, groups))
    goto done;
  REPLACE_READER (stsz, sz);
  REPLACE_READER (stts, ts);
  REPLACE_READER (stco, co);
  REPLACE_READER (stss, ss);
#undef REPLACE_READER
  gst_byte_reader_skip_unchecked (&stream->stco, 8);
  /* Every new sample is also a chunk; one stsc run describes them all. */
  g_free ((gpointer) stream->stsc.data);
  gst_byte_reader_init (&stream->stsc, g_malloc (12), 12);
  GST_WRITE_UINT32_BE ((guint8 *) stream->stsc.data, 1);
  GST_WRITE_UINT32_BE ((guint8 *) stream->stsc.data + 4, 1);
  GST_WRITE_UINT32_BE ((guint8 *) stream->stsc.data + 8, 1);
  GST_INFO_OBJECT (demux, "TrueHD index: %u access units -> %u contiguous groups",
      stream->n_samples, groups);
  stream->n_samples = groups;
  stream->sample_size = 0;
  stream->co_size = 8;
  stream->n_samples_per_chunk = 1;
  stream->n_sample_times = groups;
  stream->stss_present = TRUE;
  stream->n_sample_syncs = sync_count;
  ok = TRUE;
done:
  gst_byte_writer_reset (&sz);
  gst_byte_writer_reset (&ts);
  gst_byte_writer_reset (&co);
  gst_byte_writer_reset (&ss);
  return ok;
}
