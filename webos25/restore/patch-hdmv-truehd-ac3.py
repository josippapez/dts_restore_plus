#!/usr/bin/env python3
"""Expose separate TrueHD and AC-3 tracks for an HDMV type 0x83 entry.

The PMT list and PID table describe the transport. Publish the logical AC-3
slot immediately after TrueHD, matching FFmpeg's order even for empty slots.
"""
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
s = path.read_text()
if 'ac3_companion' in s:
    raise SystemExit('HDMV AC3 patch already applied')


def once(before, after):
    global s
    if s.count(before) != 1:
        raise SystemExit('HDMV AC3 anchor mismatch: ' + repr(before))
    s = s.replace(before, after, 1)


once('  MpegTSBaseStream stream;\n', '''  MpegTSBaseStream stream;

  /* HDMV 0x83 has an independent AC-3 PES reassembler on the same PID. */
  TSDemuxStream *ac3_companion;
  GList ac3_node;
''')
# All existing tsdemux output operations (segments, seeks, EOS, timestamps,
# collection events) must visit the companion immediately after TrueHD
# audio. The embedded node avoids changing the physical list or allocating it.
anchor = 'static gboolean\ngst_ts_demux_srcpad_event ('
once(anchor, '''static GList *
gst_ts_demux_next_output (GList * node)
{
  TSDemuxStream *stream = node->data;
  if (stream->ac3_companion && stream->ac3_companion->pad) {
    stream->ac3_node.data = stream->ac3_companion;
    stream->ac3_node.next = node->next;
    return &stream->ac3_node;
  }
  return node->next;
}

''' + anchor)
# The helper is used before its definition by earlier seek/event functions.
once('struct _TSDemuxStream\n{', '''#define HDMV_AC3_COMPAT_SUBSTREAM 0x76

static GList *gst_ts_demux_next_output (GList * node);

struct _TSDemuxStream
{''')
s, count = re.subn(r'(for \((\w+) = [\w>.-]+stream_list; \2; )\2 = (?:\2->next|g_list_next \(\2\))(\))',
                  lambda m: m[1] + m[2] + ' = gst_ts_demux_next_output (' + m[2] + ')' + m[3], s)
if count != 19:
    raise SystemExit(f'HDMV AC3 output-loop count mismatch: {count}')
# SCTE-35 routing only matches the physical section PID. Keep that lookup on
# the PMT list, as the AC-3 slot shares its parent's PID.
scte_pid_loop = '''      for (tmp = demux->program->stream_list; tmp; tmp = %s) {
        TSDemuxStream *stream = (TSDemuxStream *) tmp->data;

        if (stream->stream.pid == section->pid) {'''
once(scte_pid_loop % 'gst_ts_demux_next_output (tmp)', scte_pid_loop % 'tmp->next')
once('''  g_list_foreach (demux->program->stream_list,
      (GFunc) gst_ts_demux_stream_reset, NULL);''', '''  GList *node;
  for (node = demux->program->stream_list; node;
      node = gst_ts_demux_next_output (node))
    gst_ts_demux_stream_reset (node->data);''')
# The logical slot borrows the physical entry's descriptors. The PES substream
# identifies it as AC-3 even if the TrueHD PMT entry lacks an AC-3 descriptor.
once('''        if (ac3_desc && DESC_AC_AUDIO_STREAM_bsid (ac3_desc->data) != 16) {
          GST_LOG_OBJECT (demux, "ac3 audio");''', '''        if (stream->target_pes_substream == HDMV_AC3_COMPAT_SUBSTREAM ||
            (ac3_desc && DESC_AC_AUDIO_STREAM_bsid (ac3_desc->data) != 16)) {
          GST_LOG_OBJECT (demux, "ac3 audio");''')
once('''  return (stream->pad != NULL);
}''', '''  if (stream->pad && !stream->ac3_companion &&
      program->registration_id == DRF_ID_HDMV &&
      bstream->stream_type == ST_BD_AUDIO_AC3_TRUE_HD) {
    TSDemuxStream *ac3 = g_new0 (TSDemuxStream, 1);
    ac3->stream.pid = bstream->pid;
    ac3->stream.stream_type = ST_BD_AUDIO_AC3;
    ac3->stream.registration_id = bstream->registration_id;
    ac3->stream.stream = bstream->stream;
    ac3->stream.stream_id = g_strdup_printf ("%s/ac3", bstream->stream_id);
    ac3->stream.stream_object = gst_stream_new (ac3->stream.stream_id,
        NULL, GST_STREAM_TYPE_AUDIO, GST_STREAM_FLAG_NONE);
    ac3->target_pes_substream = HDMV_AC3_COMPAT_SUBSTREAM;
    ac3->stream.in_collection = gst_ts_demux_stream_added (base,
        &ac3->stream, program);
    stream->ac3_companion = ac3;
  }
  return (stream->pad != NULL);
}''')
once('''  if (stream->pad) {
    gst_flow_combiner_remove_pad''', '''  if (stream->ac3_companion) {
    TSDemuxStream *ac3 = stream->ac3_companion;
    stream->ac3_companion = NULL;
    gst_ts_demux_stream_removed (base, &ac3->stream);
    gst_object_unref (ac3->stream.stream_object);
    g_free (ac3->stream.stream_id);
    g_free (ac3);
  }

  if (stream->pad) {
    gst_flow_combiner_remove_pad''')
once('''static void
gst_ts_demux_update_program (MpegTSBase * base, MpegTSBaseProgram * program)
{''', '''/* Preserve PMT order with the HDMV AC3 slot immediately after TrueHD. */
static void
gst_ts_demux_collect_ac3 (MpegTSBaseProgram * program)
{
  GList *node;
  gboolean have_ac3 = FALSE;
  GstStreamCollection *collection;
  for (node = program->stream_list; node; node = node->next) {
    TSDemuxStream *stream = node->data;
    if (stream->ac3_companion) {
      stream->ac3_companion->stream.stream = stream->stream.stream;
      have_ac3 |= stream->ac3_companion->pad != NULL;
    }
  }
  if (!have_ac3)
    return;
  collection = gst_stream_collection_new (
      gst_stream_collection_get_upstream_id (program->collection));
  for (node = program->stream_list; node;
      node = gst_ts_demux_next_output (node)) {
    TSDemuxStream *stream = node->data;
    if (stream->stream.in_collection) {
      gst_stream_collection_add_stream (collection,
          gst_object_ref (stream->stream.stream_object));
    }
  }
  gst_object_unref (program->collection);
  program->collection = collection;
}

static void
gst_ts_demux_update_program (MpegTSBase * base, MpegTSBaseProgram * program)
{''')
once('''  GST_DEBUG ("Updating program %d", program->program_number);''', '''  gst_ts_demux_collect_ac3 (program);
  GST_DEBUG ("Updating program %d", program->program_number);''')
once('''    /* Emit collection message */
    gst_element_post_message''', '''    gst_ts_demux_collect_ac3 (program);
    /* Emit collection message */
    gst_element_post_message''')
once('''  if (stream->target_pes_substream != 0
      && header.stream_id_extension != stream->target_pes_substream) {''', '''  if (stream->target_pes_substream != 0
      && header.stream_id_extension != stream->target_pes_substream &&
      /* tsMuxer can carry a TrueHD-only HDMV 0x83 PID in private_stream_1
       * without a PES extension. Keep accepting its TrueHD bytes, while
       * the independently advertised AC3 slot remains empty. */
      !(stream->ac3_companion && stream->target_pes_substream == 0x72 &&
          header.stream_id == ID_PRIVATE_STREAM_1 && length >= 9 &&
          !(data[7] & 0x01))) {''')
once('''      res = gst_ts_demux_handle_packet (demux, stream, packet, section);''', '''      res = gst_ts_demux_handle_packet (demux, stream, packet, section);
      /* Each output filters its own PES substream and uses the same flow
       * combiner. Give the AC-3 output its turn when TrueHD is unlinked. */
      if (stream->ac3_companion &&
          (res == GST_FLOW_OK || res == GST_FLOW_NOT_LINKED))
        res = gst_ts_demux_handle_packet (demux, stream->ac3_companion,
            packet, section);''')
once('''        gst_pad_push_event (ps->pad, gst_event_new_gap (time, 0));''', '''        GstClockTime gap_time = time;
        /* An empty compatibility slot has no audio clock of its own. PCR
         * follows demux read-ahead and can be many seconds ahead of playback;
         * a future GAP blocks LG's audio sink and delays a subsequent switch
         * back to TrueHD. Only clamp the never-buffered AC3 companion. */
        if (ps->target_pes_substream == HDMV_AC3_COMPAT_SUBSTREAM &&
            ps->nb_out_buffers == 0 &&
            ps->raw_pts == -1 && ps->raw_dts == -1 &&
            base->out_segment.format == GST_FORMAT_TIME) {
          GstClock *clock = gst_element_get_clock (GST_ELEMENT (demux));
          if (clock) {
            GstClockTime now = gst_clock_get_time (clock);
            GstClockTime start = gst_element_get_base_time (GST_ELEMENT (demux));
            if (GST_CLOCK_TIME_IS_VALID (start) && now >= start) {
              GstClockTime position = gst_segment_position_from_running_time (
                  &base->out_segment, GST_FORMAT_TIME, now - start);
              if (GST_CLOCK_TIME_IS_VALID (position))
                gap_time = MIN (gap_time, position);
            }
            gst_object_unref (clock);
          }
        }
        gst_pad_push_event (ps->pad, gst_event_new_gap (gap_time, 0));''')
path.write_text(s)
print('HDMV TrueHD: independent TrueHD/AC3 outputs in FFmpeg PMT order')
