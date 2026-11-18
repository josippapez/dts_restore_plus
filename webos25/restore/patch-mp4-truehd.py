#!/usr/bin/env python3
"""Add TrueHD-in-MP4 support to LG's GStreamer 1.24 qtdemux."""
from pathlib import Path
import sys


def patch_moov_limit(source):
    # Long TrueHD tracks can have millions of entries in their sample tables.
    # Raise the metadata limit only for moov, in push and pull mode.
    def replace(before, after):
        nonlocal source
        if source.count(before) != 1:
            raise ValueError('MP4 moov anchor mismatch: ' + repr(before))
        source = source.replace(before, after, 1)

    replace('#define QTDEMUX_MAX_ATOM_SIZE (32*1024*1024)',
            '#define QTDEMUX_MAX_ATOM_SIZE (32*1024*1024)\n'
            '/* Long TrueHD tracks can have large sample tables. Keep a hard bound. */\n'
            '#define QTDEMUX_MAX_MOOV_SIZE (128*1024*1024)')
    replace('''        } else if (G_UNLIKELY (size > QTDEMUX_MAX_ATOM_SIZE)) {''',
            '''        } else if (G_UNLIKELY (size > (fourcc == FOURCC_moov ?
                    QTDEMUX_MAX_MOOV_SIZE : QTDEMUX_MAX_ATOM_SIZE))) {''')
    # Apply the same bound to ordinary and size-to-EOF moov boxes in pull mode.
    replace('''        length = duration - cur_offset;
        if (length > QTDEMUX_MAX_ATOM_SIZE) {
          GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
              (_("Cannot demux file")),
              ("Moov atom size %" G_GINT64_FORMAT " > maximum %d", length,
                  QTDEMUX_MAX_ATOM_SIZE));
          ret = GST_FLOW_ERROR;
          goto beach;
        }
      }
''', '''        length = duration - cur_offset;
      }
      if (length > QTDEMUX_MAX_MOOV_SIZE) {
        GST_ELEMENT_ERROR (qtdemux, STREAM, DEMUX,
            (_("Cannot demux file")),
            ("Moov atom size %" G_GUINT64_FORMAT " > maximum %d", length,
                QTDEMUX_MAX_MOOV_SIZE));
        ret = GST_FLOW_ERROR;
        goto beach;
      }
''')
    return source


def patch_truehd_samples(source):
    # MP4 identifies Dolby TrueHD audio with the mlpa sample entry.
    before = '''    case FOURCC_ac_4:
      _codec ("AC-4 audio");'''
    after = '''    case GST_MAKE_FOURCC ('m', 'l', 'p', 'a'):
      _codec ("Dolby TrueHD");
      caps = gst_caps_new_simple ("audio/x-true-hd",
          "framed", G_TYPE_BOOLEAN, TRUE, NULL);
      /* The sample entry may contain placeholder channels/rate. The TrueHD
       * major sync carries the real values; timestamps use the MP4 timescale. */
      entry->n_channels = 0;
      entry->rate = 0;
      entry->sampled = TRUE;
      break;
''' + before
    if source.count(before) != 1 or 'case GST_MAKE_FOURCC (\'m\', \'l\', \'p\', \'a\'):' in source:
        raise ValueError('MP4 TrueHD anchor mismatch or already applied')
    source = source.replace(before, after, 1)
    anchor = '/* initialise bytereaders for stbl sub-atoms */'
    if source.count(anchor) != 1:
        raise ValueError('MP4 TrueHD index include anchor mismatch')
    source = source.replace(anchor, '#include "qtdemux-truehd-index.h"\n\n' + anchor, 1)
    anchor = '''  if (stream->min_buffer_size > 0) {
    qtdemux_merge_sample_table (qtdemux, stream);
  }
'''
    if source.count(anchor) != 1:
        raise ValueError('MP4 TrueHD sample table anchor mismatch')
    # Compact before qtdemux allocates its bounded per-sample index. A table the
    # helper rejects is left untouched, so the ordinary index still gets a try.
    return source.replace(anchor, anchor + '''  if (CUR_STREAM (stream)->fourcc == GST_MAKE_FOURCC ('m', 'l', 'p', 'a') &&
      !qtdemux_compact_truehd_index (qtdemux, stream))
    GST_INFO_OBJECT (qtdemux, "TrueHD index not compacted, using the ordinary index");
''', 1)


def patch_chunk_range(source):
    # Index compaction always writes co64, which the stock range read below
    # misreads as stco. Read TrueHD tables at their real width and leave every
    # other track on the stock read, so non-TrueHD files behave as before.
    before = """  if (stream->stco.data != NULL) {
    guint32 entry_num;

    entry_num = QT_UINT32 (stream->stco.data + stream->co_size);
"""
    after = """  if (stream->stco.data != NULL &&
      CUR_STREAM (stream)->fourcc == GST_MAKE_FOURCC ('m', 'l', 'p', 'a')) {
    GstByteReader chunks = stream->stco;
    guint32 entry_num;

    stream->first_chunk_offset = 0;
    stream->last_chunk_offset = 0;
    /* The entry count follows the 4-byte version and flags in stco and co64. */
    if (gst_byte_reader_set_pos (&chunks, 4) &&
        gst_byte_reader_get_uint32_be (&chunks, &entry_num) &&
        entry_num > 1 &&
        entry_num <= gst_byte_reader_get_remaining (&chunks) / stream->co_size) {
      stream->first_chunk_offset =
          qt_atom_parser_get_offset_unchecked (&chunks, stream->co_size);
      gst_byte_reader_skip_unchecked (&chunks, (entry_num - 2) * stream->co_size);
      stream->last_chunk_offset =
          qt_atom_parser_get_offset_unchecked (&chunks, stream->co_size);
      GST_DEBUG_OBJECT (qtdemux,
          "(stream %" GST_FOURCC_FORMAT ") first offset %" G_GINT64_FORMAT
          ", last offset %" G_GINT64_FORMAT, GST_FOURCC_ARGS (stream->subtype),
          stream->first_chunk_offset, stream->last_chunk_offset);
    }
  } else if (stream->stco.data != NULL) {
    guint32 entry_num;

    entry_num = QT_UINT32 (stream->stco.data + stream->co_size);
"""
    if source.count(before) != 1:
        raise ValueError("MP4 chunk offset range anchor mismatch")
    return source.replace(before, after, 1)


if __name__ == "__main__":
    path = Path(sys.argv[1])
    # The index helper also uses QTDEMUX_MAX_MOOV_SIZE to bound its input.
    # Check every anchor and read the header before writing to the build tree.
    source = path.read_text(encoding="utf-8")
    result = patch_chunk_range(patch_truehd_samples(patch_moov_limit(source)))
    header = Path(__file__).parent / "src/qtdemux-truehd-index.h"
    path.with_name(header.name).write_bytes(header.read_bytes())
    path.write_bytes(result.encode("utf-8"))
    print("MP4: framed TrueHD, compact sample index and 128 MiB moov limit")
