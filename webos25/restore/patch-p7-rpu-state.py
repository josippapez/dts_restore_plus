#!/usr/bin/env python3
"""Normalize Dolby Vision profile 7 metadata in the copied HEVC parser."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
directory = root / 'gst-plugins-bad/gst/videoparsers'
source = directory / 'gsth265parse.c'
text = source.read_text()
if 'dovi-rpu-normalize.h' in text:
    raise SystemExit('P7 RPU state patch already applied')

def replace(before, after):
    global text
    if text.count(before) != 1:
        raise SystemExit('P7 RPU state anchor mismatch: ' + repr(before))
    text = text.replace(before, after, 1)

replace('#define GST_CAT_DEFAULT h265_parse_debug',
        '#define GST_CAT_DEFAULT h265_parse_debug\n#include "dovi-rpu-normalize.h"')
replace('''  h265parse->has_dolby_vision_field = FALSE;''', '''  h265parse->has_dolby_vision_field = FALSE;
  g_object_set_qdata_full (G_OBJECT (h265parse), dovi_state_key (), NULL, NULL);
  g_object_set_data (G_OBJECT (h265parse), "dts-restore-p7-bl-enabled", NULL);''')
replace('''  /* Dolby HDR */
  if (gst_structure_has_field (str, "dolby-vision"))''', '''  /* The profile 7 decoder uses the base layer even when the input has
   * separate encoded layers. Normalize the RPU-bearing track before any
   * layer assembly. A combiner can join encoded tracks without supplying
   * a decoded enhancement-layer residual. */
  {
    gboolean enabled = dovi_bl_rpu_required (str);
    g_object_set_data (G_OBJECT (h265parse), "dts-restore-p7-bl-enabled",
        GINT_TO_POINTER (enabled));
    if (enabled)
      gst_base_parse_set_passthrough (parse, FALSE);
  }
  /* Dolby HDR */
  if (gst_structure_has_field (str, "dolby-vision"))''')
replace('''  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:''', '''  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
      g_object_set_qdata_full (G_OBJECT (h265parse), dovi_state_key (), NULL, NULL);
      res = GST_BASE_PARSE_CLASS (parent_class)->sink_event (parse, event);
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM:''')
replace('''  gst_video_push_user_data_unregistered ((GstElement *) h265parse,
      &h265parse->user_data_unregistered, parse_buffer);

  gst_h265_parse_reset_frame (h265parse);''', '''  gst_video_push_user_data_unregistered ((GstElement *) h265parse,
      &h265parse->user_data_unregistered, parse_buffer);

  if (g_object_get_data (G_OBJECT (h265parse), "dts-restore-p7-bl-enabled")) {
    GstBuffer *normalized = dovi_normalize_buffer (GST_ELEMENT (h265parse),
        parse_buffer, h265parse->format == GST_H265_PARSE_FORMAT_BYTE,
        h265parse->nal_length_size);
    if (!normalized)
      return GST_FLOW_ERROR;
    gst_buffer_replace (&frame->out_buffer, normalized);
    gst_buffer_unref (normalized);
  }
  gst_h265_parse_reset_frame (h265parse);''')

meson = directory / 'meson.build'
build = meson.read_text()
anchor = 'dependencies : [gstcodecparsers_dep, gstbase_dep, gstpbutils_dep, gstvideo_dep]'
if build.count(anchor) != 1:
    raise SystemExit('P7 RPU state meson anchor mismatch')
build = build.replace(anchor, anchor[:-1] + ", cc.find_library('dl')]")
source.write_text(text)
meson.write_text(build)
(directory / 'dovi-rpu-normalize.h').write_bytes(
    (Path(__file__).parent / 'src/dovi-rpu-normalize.h').read_bytes())
print('Dolby Vision P7 patch: base-layer RPU normalization and per-stream active area')
