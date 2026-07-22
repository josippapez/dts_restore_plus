/* gstbdlpcmenc.c - Blu-ray/HDMV LPCM audio encoder element
 *
 * Element name: "bdlpcmenc"
 *
 * Takes interleaved raw PCM (S16/S24, LE or BE) and re-frames it as
 * HDMV/Blu-ray LPCM: a 4-byte big-endian header followed by big-endian
 * interleaved samples in BD channel order (LFE LAST). The output caps
 * are the (deliberately empty) "audio/x-private-ts-lpcm" that LG's
 * decproxy/fakedec whitelist still accepts, so decodebin autoplugs it
 * into the proprietary multichannel LPCM decoder (pcm_audiodec).
 *
 * Wire format is bit-identical to gst-plugins-good gstdvdlpcmdec.c in
 * BLURAY mode and to ffmpeg's pcm-bluray codec.
 *
 * License: LGPL v2.1 or later (see header).
 *
 * Targets the GStreamer 1.14.x API used by LG webOS (OLED CX). All
 * GstAudioEncoder / GstAudioInfo APIs used here exist since 1.0, so no
 * 1.16+-only symbols are referenced.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "gstbdlpcmenc.h"

GST_DEBUG_CATEGORY_STATIC (bdlpcmenc_debug);
#define GST_CAT_DEFAULT bdlpcmenc_debug

/* BD-LPCM does not permit 44100 Hz: only 48/96/192 kHz. Upstream must
 * resample 44.1k -> 48k (the dtstolpcm bin inserts audioresample). */
static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) { S16LE, S16BE, S24LE, S24BE }, "
        "layout = (string) interleaved, "
        "rate = (int) { 48000, 96000, 192000 }, "
        "channels = (int) [ 1, 8 ]"));

/* LG's tsdemux emits EMPTY caps for private-ts-lpcm and the format
 * travels in-band inside the 4-byte header; we mirror that here by
 * emitting a bare, field-less "audio/x-private-ts-lpcm". Adding
 * channels/rate fields would break autoplug matching against the
 * whitelist entry. */
static GstStaticPadTemplate src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-private-ts-lpcm"));

#define gst_bdlpcm_enc_parent_class parent_class
G_DEFINE_TYPE (GstBdLpcmEnc, gst_bdlpcm_enc, GST_TYPE_AUDIO_ENCODER);

/* -- reorder helpers ------------------------------------------------------
 *
 * BD channel order (what we WRITE) with LFE last:
 *   2ch: L, R
 *   6ch: L, R, C, Ls, Rs, LFE
 *   8ch: L, R, C, Lside, Ls, Rs, Rside, LFE
 * (7.1 source order confirmed against ffmpeg pcm-bluray.c.)
 *
 * GStreamer's SMPTE interleave order (what avdec_dca PRODUCES):
 *   6ch: FL, FR, FC, LFE, RL, RR
 *   8ch: FL, FR, FC, LFE, RL, RR, SL, SR
 */

/* Position lists describing the BD output slot -> speaker mapping. */
static const GstAudioChannelPosition bd_pos_2[2] = {
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
};
static const GstAudioChannelPosition bd_pos_6[6] = {
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
  GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
  GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_LFE1,
};
static const GstAudioChannelPosition bd_pos_8[8] = {
  GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
  GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
  GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
  GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
  GST_AUDIO_CHANNEL_POSITION_LFE1,
};

/* Static fallback maps (index into the SMPTE-ordered input) used when the
 * incoming GstAudioInfo carries no valid channel positions. */
static const gint bd_fallback_2[2] = { 0, 1 };
static const gint bd_fallback_6[6] = { 0, 1, 2, 4, 5, 3 };
static const gint bd_fallback_8[8] = { 0, 1, 2, 6, 4, 5, 7, 3 };

static gboolean
bdlpcm_positions_valid (const GstAudioInfo * info)
{
  gint i, ch = GST_AUDIO_INFO_CHANNELS (info);
  for (i = 0; i < ch; i++) {
    GstAudioChannelPosition p = info->position[i];
    if (p == GST_AUDIO_CHANNEL_POSITION_NONE ||
        p == GST_AUDIO_CHANNEL_POSITION_INVALID ||
        p == GST_AUDIO_CHANNEL_POSITION_MONO ||
        p == GST_AUDIO_CHANNEL_POSITION_UNPOSITIONED)
      return FALSE;
  }
  return TRUE;
}

static gint
bdlpcm_find_position (const GstAudioInfo * info, GstAudioChannelPosition want)
{
  gint i, ch = GST_AUDIO_INFO_CHANNELS (info);
  for (i = 0; i < ch; i++)
    if (info->position[i] == want)
      return i;
  return -1;
}

static void
bdlpcm_build_reorder (GstBdLpcmEnc * self, const GstAudioInfo * info)
{
  const GstAudioChannelPosition *target;
  const gint *fallback;
  gboolean use_pos;
  gint c, in_ch = GST_AUDIO_INFO_CHANNELS (info);

  switch (self->bd_channels) {
    case 6:
      target = bd_pos_6;
      fallback = bd_fallback_6;
      break;
    case 8:
      target = bd_pos_8;
      fallback = bd_fallback_8;
      break;
    case 2:
    default:
      target = bd_pos_2;
      fallback = bd_fallback_2;
      break;
  }

  use_pos = bdlpcm_positions_valid (info);

  for (c = 0; c < self->bd_channels; c++) {
    gint idx = -1;

    if (use_pos)
      idx = bdlpcm_find_position (info, target[c]);

    if (idx < 0) {
      /* Fall back to fixed SMPTE-order assumption; if that input channel
       * does not exist (padded layout), emit silence for this BD slot. */
      gint f = fallback[c];
      idx = (f < in_ch) ? f : -1;
    }

    self->reorder[c] = idx;
  }
}

/* -- GstAudioEncoder vmethods -------------------------------------------- */

static gboolean
gst_bdlpcm_enc_start (GstAudioEncoder * enc)
{
  GstBdLpcmEnc *self = GST_BDLPCM_ENC (enc);
  self->configured = FALSE;
  return TRUE;
}

static gboolean
gst_bdlpcm_enc_stop (GstAudioEncoder * enc)
{
  GstBdLpcmEnc *self = GST_BDLPCM_ENC (enc);
  self->configured = FALSE;
  return TRUE;
}

static gboolean
gst_bdlpcm_enc_set_format (GstAudioEncoder * enc, GstAudioInfo * info)
{
  GstBdLpcmEnc *self = GST_BDLPCM_ENC (enc);
  GstCaps *out_caps;
  gint rate, depth, in_ch;
  const GstAudioFormatInfo *finfo;

  in_ch = GST_AUDIO_INFO_CHANNELS (info);
  rate = GST_AUDIO_INFO_RATE (info);
  finfo = info->finfo;
  depth = GST_AUDIO_FORMAT_INFO_DEPTH (finfo);

  /* sample-rate nibble */
  switch (rate) {
    case 48000:  self->sr_code = 1; break;
    case 96000:  self->sr_code = 4; break;
    case 192000: self->sr_code = 5; break;
    default:
      GST_ERROR_OBJECT (self, "rate %d not valid for BD-LPCM "
          "(need 48000/96000/192000)", rate);
      return FALSE;
  }

  /* bits-per-sample nibble + framing */
  switch (depth) {
    case 16:
      self->bps_code = 1;
      self->bytes_per_sample = 2;
      self->frame_samples = 240;    /* matches dvdlpcmdec/pcm_bluray */
      break;
    case 24:
      self->bps_code = 3;
      self->bytes_per_sample = 3;   /* 24-bit carried as 3 bytes/sample */
      self->frame_samples = 360;
      break;
    default:
      GST_ERROR_OBJECT (self, "depth %d not supported (need 16 or 24)", depth);
      return FALSE;
  }

  /* channel-assignment nibble + target BD layout (pad up to next even) */
  if (in_ch <= 2) {
    self->bd_channels = 2;
    self->ca_code = 3;
  } else if (in_ch <= 6) {
    self->bd_channels = 6;
    self->ca_code = 9;
  } else {
    self->bd_channels = 8;
    self->ca_code = 11;
  }

  self->in_channels = in_ch;
  self->swap = (GST_AUDIO_FORMAT_INFO_ENDIANNESS (finfo) != G_BIG_ENDIAN);
  self->info = *info;

  bdlpcm_build_reorder (self, info);

  /* Hand the base class fixed-size frames so we always pack whole BD
   * frames; the tail (< frame_samples) is flushed on EOS and emitted as a
   * short frame whose size field reflects the real payload. */
  gst_audio_encoder_set_frame_samples_min (enc, self->frame_samples);
  gst_audio_encoder_set_frame_samples_max (enc, self->frame_samples);
  gst_audio_encoder_set_frame_max (enc, 1);
  gst_audio_encoder_set_hard_min (enc, FALSE);

  /* Field-less caps on purpose (see src_template comment). */
  out_caps = gst_caps_new_empty_simple ("audio/x-private-ts-lpcm");
  if (!gst_audio_encoder_set_output_format (enc, out_caps)) {
    GST_ERROR_OBJECT (self, "failed to set output format");
    gst_caps_unref (out_caps);
    return FALSE;
  }
  gst_caps_unref (out_caps);

  self->configured = TRUE;

  GST_INFO_OBJECT (self, "configured: in_ch=%d bd_ch=%d ca=%d sr=%d bps=%d "
      "bytes/sample=%d frame_samples=%d swap=%d",
      in_ch, self->bd_channels, self->ca_code, self->sr_code,
      self->bps_code, self->bytes_per_sample, self->frame_samples, self->swap);

  return TRUE;
}

static GstFlowReturn
gst_bdlpcm_enc_handle_frame (GstAudioEncoder * enc, GstBuffer * buffer)
{
  GstBdLpcmEnc *self = GST_BDLPCM_ENC (enc);
  GstMapInfo in_map, out_map;
  GstBuffer *outbuf;
  guint8 *dst;
  const guint8 *src;
  gint in_bpf, bps, bd_ch, in_ch;
  gint n_samples, s, c;
  guint payload;

  /* NULL buffer => EOS drain with no residue. */
  if (G_UNLIKELY (buffer == NULL))
    return GST_FLOW_OK;

  if (G_UNLIKELY (!self->configured))
    return GST_FLOW_NOT_NEGOTIATED;

  bps = self->bytes_per_sample;
  bd_ch = self->bd_channels;
  in_ch = self->in_channels;
  in_bpf = in_ch * bps;         /* input bytes per (multi-channel) sample */

  if (!gst_buffer_map (buffer, &in_map, GST_MAP_READ))
    return GST_FLOW_ERROR;

  n_samples = (gint) (in_map.size / in_bpf);
  if (n_samples <= 0) {
    gst_buffer_unmap (buffer, &in_map);
    /* nothing to emit but consume what was handed to us */
    return gst_audio_encoder_finish_frame (enc, NULL, -1);
  }

  payload = (guint) n_samples * bd_ch * bps;    /* excludes 4-byte header */

  outbuf = gst_buffer_new_allocate (NULL, 4 + payload, NULL);
  if (!outbuf) {
    gst_buffer_unmap (buffer, &in_map);
    return GST_FLOW_ERROR;
  }

  if (!gst_buffer_map (outbuf, &out_map, GST_MAP_WRITE)) {
    gst_buffer_unref (outbuf);
    gst_buffer_unmap (buffer, &in_map);
    return GST_FLOW_ERROR;
  }

  dst = out_map.data;
  src = in_map.data;

  /* 4-byte big-endian header (see wire-format spec) */
  dst[0] = (guint8) ((payload >> 8) & 0xff);
  dst[1] = (guint8) (payload & 0xff);
  dst[2] = (guint8) ((self->ca_code << 4) | (self->sr_code & 0x0f));
  dst[3] = (guint8) ((self->bps_code & 0x03) << 6);
  dst += 4;

  /* Interleaved, big-endian, BD channel order (LFE last). */
  for (s = 0; s < n_samples; s++) {
    const guint8 *frame = src + (gsize) s * in_bpf;
    for (c = 0; c < bd_ch; c++) {
      gint in_idx = self->reorder[c];
      if (in_idx < 0) {
        /* padded/absent BD slot -> silence */
        memset (dst, 0, bps);
      } else {
        const guint8 *sp = frame + (gsize) in_idx * bps;
        if (self->swap) {
          /* input LE -> emit BE: reverse the sample bytes */
          gint b;
          for (b = 0; b < bps; b++)
            dst[b] = sp[bps - 1 - b];
        } else {
          memcpy (dst, sp, bps);
        }
      }
      dst += bps;
    }
  }

  gst_buffer_unmap (outbuf, &out_map);
  gst_buffer_unmap (buffer, &in_map);

  /* finish_frame timestamps the output from the consumed input samples. */
  return gst_audio_encoder_finish_frame (enc, outbuf, n_samples);
}

static void
gst_bdlpcm_enc_class_init (GstBdLpcmEncClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAudioEncoderClass *base_class = GST_AUDIO_ENCODER_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  gst_element_class_set_static_metadata (element_class,
      "Blu-ray/HDMV LPCM encoder",
      "Codec/Encoder/Audio",
      "Re-frames raw PCM as HDMV/Blu-ray LPCM (audio/x-private-ts-lpcm)",
      "dts_restore contributors");

  base_class->start = GST_DEBUG_FUNCPTR (gst_bdlpcm_enc_start);
  base_class->stop = GST_DEBUG_FUNCPTR (gst_bdlpcm_enc_stop);
  base_class->set_format = GST_DEBUG_FUNCPTR (gst_bdlpcm_enc_set_format);
  base_class->handle_frame = GST_DEBUG_FUNCPTR (gst_bdlpcm_enc_handle_frame);

  GST_DEBUG_CATEGORY_INIT (bdlpcmenc_debug, "bdlpcmenc", 0,
      "Blu-ray/HDMV LPCM encoder");
}

static void
gst_bdlpcm_enc_init (GstBdLpcmEnc * self)
{
  self->configured = FALSE;
  gst_audio_info_init (&self->info);
}
