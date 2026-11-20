/* dtsdec for LG webOS 6 / GStreamer 1.16 (OLED C1/G1, the C1 profile of the
 * repo-root init_dts.sh and of the app): DTS core -> IEC 61937 for an
 * optical/ARC receiver, with an FFmpeg dca stereo decode as the safe fallback.
 *
 * Copyright (C) 2004 Ronald Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2009 Jan Schmidt <thaytan@noraisin.net>
 * webOS 6 port: dts_restore_plus contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/* ==========================================================================
 * Why a separate source and not ../gstdtsdec.c
 * --------------------------------------------------------------------------
 * ../gstdtsdec.c is the on-device-verified webOS 25 (C5) decoder. Any change
 * to it is a binary-affecting change that needs a C5 re-verification
 * (CLAUDE.md rule 1), and most of it is C5-only: lgroute.c drives LG's 1.24
 * audiosink (pcm-multi-ch, the MAT carrier caps, ALSA mixer controls), none
 * of which exist on webOS 6. This file keeps the two pieces that do apply,
 * copied verbatim and unchanged:
 *
 *   - the DTS frame sizer (core + DTS-HD EXSS), and
 *   - dts_iec61937_pack(), the IEC 61937 packer that is byte-identical to
 *     FFmpeg's spdif muxer in its default (little-endian, core-only) mode and
 *     is covered by ../test/test-iec61937.c against iec61937-dts.ref.
 *
 * Evidence this is enough on a C1 (OLED65C17LB, 03.53.45, optical receiver,
 * Digital Sound Output = Pass Through): an MKV whose PCM track was exactly
 * those bytes (S16LE, 2 ch, 48 kHz) made the receiver show DTS and decode it.
 * So LG's webOS 6 video path hands 48 kHz S16LE stereo to S/PDIF bit-exact,
 * and passthrough needs nothing but plain audio/x-raw caps.
 *
 * Output decision, made once per stream (at the first frame after start or a
 * new STREAM_START):
 *
 *   passthrough  only if ALL hold:
 *     - mode is "auto" and the opt-in flag file exists, or mode is
 *       "passthrough" (property or DTSDEC_OUTPUT env, for tests);
 *     - in "auto": the live sound-output watcher (started by the boot script)
 *       reports external_optical or external_arc AND Digital Sound Output
 *       passThrough, and its pid is alive. A missing file, a dead watcher,
 *       TV speakers or headphones mean decode: the bitstream on speakers is
 *       loud noise;
 *     - the frame is a 16-bit big-endian 48 kHz DTS core (every DTS track in
 *       MKV/MP4/TS). DTS-HD extensions are dropped: S/PDIF carries the core.
 *   decode       otherwise: FFmpeg dca -> stereo S16LE downmix at the stream
 *                rate (the format LG's webOS 6 sink is known to take).
 * ========================================================================== */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include <gst/gst.h>
#include <gst/audio/audio.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

#ifndef DTSC1_STATE_DIR
#define DTSC1_STATE_DIR "/var/lib/webosbrew/dtsc1"
#endif
#ifndef DTSC1_RUN_DIR
#define DTSC1_RUN_DIR "/run/dtsc1"
#endif
#define DTSC1_PT_FLAG      DTSC1_STATE_DIR "/passthrough"
#define DTSC1_SOUND_OUTPUT DTSC1_RUN_DIR "/sound-output"
#define DTSC1_LAST         DTSC1_RUN_DIR "/last-decision"

GST_DEBUG_CATEGORY_STATIC (dtsdec_debug);
#define GST_CAT_DEFAULT (dtsdec_debug)

#define GST_TYPE_DTSDEC (gst_dtsdec_get_type ())
#define GST_DTSDEC(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_DTSDEC, GstDtsDec))

typedef enum
{
  DTSDEC_OUTPUT_AUTO,
  DTSDEC_OUTPUT_PASSTHROUGH,
  DTSDEC_OUTPUT_DECODE
} DtsDecOutput;

typedef struct _GstDtsDec
{
  GstAudioDecoder element;

  GstPadChainFunction base_chain;
  gboolean dvdmode;

  DtsDecOutput output;          /* property */
  gboolean decided;             /* per stream */
  gboolean passthrough;         /* per stream, after the decision */
  gint pt_period;               /* bytes per burst currently negotiated */
  gboolean pt_auto;             /* passthrough chosen by the live watcher */
  gint64 pt_checked;            /* monotonic time of the last watcher check */
  gchar *stream_id;

  /* decode path */
  AVCodecContext *avctx;
  AVFrame *frame;
  AVPacket *pkt;
  guint8 *pktbuf;
  gsize pktbuf_size;
  gint dec_rate;                /* negotiated decode rate, 0 = none */
  gboolean tagged;
} GstDtsDec;

typedef struct _GstDtsDecClass
{
  GstAudioDecoderClass parent_class;
} GstDtsDecClass;

enum
{
  PROP_0,
  PROP_OUTPUT
};

static GType
gst_dtsdec_output_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {DTSDEC_OUTPUT_AUTO, "Passthrough when opted in and a receiver is the "
          "live sound output, otherwise decode", "auto"},
    {DTSDEC_OUTPUT_PASSTHROUGH, "Always IEC 61937 (testing only)",
        "passthrough"},
    {DTSDEC_OUTPUT_DECODE, "Always decode to stereo PCM", "decode"},
    {0, NULL, NULL}
  };
  if (!type)
    type = g_enum_register_static ("GstDtsDec116Output", values);
  return type;
}

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts; "
        "audio/x-unknown, codec-id=(string)A_DTS"));

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, format=(string)S16LE, "
        "layout=(string)interleaved, rate=(int)[ 8000, 192000 ], "
        "channels=(int)2, channel-mask=(bitmask)0x3"));

GType gst_dtsdec_get_type (void);
G_DEFINE_TYPE (GstDtsDec, gst_dtsdec, GST_TYPE_AUDIO_DECODER);

/* ---------------------------------------------------------------------------
 * DTS frame sizing -- verbatim from ../gstdtsdec.c (webOS 25 patch 4).
 * ------------------------------------------------------------------------- */
#define DTS_SYNC_CORE_BE      0x7FFE8001u
#define DTS_SYNC_CORE_LE      0xFE7F0180u
#define DTS_SYNC_CORE_14B_BE  0x1FFFE800u
#define DTS_SYNC_CORE_14B_LE  0xFF1F00E8u
#define DTS_SYNC_EXSS         0x64582025u
#define DTS_MIN_CORE_SIZE     96

static guint
dts_read_be16 (const guint8 * d, gboolean le)
{
  return le ? (guint) (d[1] << 8 | d[0]) : (guint) (d[0] << 8 | d[1]);
}

static gint
dts_core_size (const guint8 * d, gint avail)
{
  guint32 sync;
  gint fsize;

  if (avail < 16)
    return -1;
  sync = GST_READ_UINT32_BE (d);

  if (sync == DTS_SYNC_CORE_BE || sync == DTS_SYNC_CORE_LE) {
    gboolean le = (sync == DTS_SYNC_CORE_LE);
    guint w2 = dts_read_be16 (d + 4, le), w3 = dts_read_be16 (d + 6, le);

    if ((w2 & 0xFC00) != 0xFC00)
      return 0;
    fsize = (gint) (((w2 & 0x3) << 12) | (w3 >> 4)) + 1;
    return fsize < DTS_MIN_CORE_SIZE ? 0 : fsize;
  }

  if (sync == DTS_SYNC_CORE_14B_BE || sync == DTS_SYNC_CORE_14B_LE) {
    gboolean le = (sync == DTS_SYNC_CORE_14B_LE);
    guint64 bits = 0;
    gint i;

    for (i = 2; i < 6; i++)
      bits = (bits << 14) | (dts_read_be16 (d + 2 * i, le) & 0x3FFF);
    if (((bits >> 46) & 0x3FF) != 0x7F)
      return 0;
    fsize = (gint) ((bits >> 24) & 0x3FFF) + 1;
    if (fsize < DTS_MIN_CORE_SIZE)
      return 0;
    return fsize * 8 / 14 * 2;
  }

  return 0;
}

static gint
dts_exss_size (const guint8 * d, gint avail)
{
  guint64 v;
  gint i;

  if (avail < 4)
    return -1;
  if (GST_READ_UINT32_BE (d) != DTS_SYNC_EXSS)
    return 0;
  if (avail < 10)
    return -1;

  for (v = 0, i = 4; i < 10; i++)
    v = (v << 8) | d[i];
  if (v & ((guint64) 1 << 37))
    return (gint) ((v >> 5) & 0xFFFFF) + 1;
  return (gint) ((v >> 13) & 0xFFFF) + 1;
}

static gint
dts_frame_size (const guint8 * d, gint avail)
{
  gint core = dts_core_size (d, avail), ext;

  if (core == -1 && avail >= 4 && GST_READ_UINT32_BE (d) == DTS_SYNC_EXSS)
    core = 0;
  if (core < 0)
    return -1;
  if (core == 0)
    return dts_exss_size (d, avail);
  if (avail < core + 4)
    return -1;
  ext = dts_exss_size (d + core, avail - core);
  if (ext < 0)
    return -1;
  return core + ext;
}

/* ---------------------------------------------------------------------------
 * IEC 61937 DTS core packer -- verbatim from ../gstdtsdec.c.
 * Byte-identical to FFmpeg's spdif muxer (default LE, core only); returns the
 * period in bytes (512/1024/2048 samples x 2 ch x 2 bytes) or 0.
 * ------------------------------------------------------------------------- */
static gint
dts_iec61937_pack (const guint8 * d, gsize size, guint8 * out, gint out_size)
{
  guint blocks, type;
  gint core, period, length, i;

  if (size < 9 || GST_READ_UINT32_BE (d) != DTS_SYNC_CORE_BE)
    return 0;
  blocks = ((GST_READ_UINT16_BE (d + 4) >> 2) & 0x7f) + 1;
  core = (gint) ((GST_READ_UINT24_BE (d + 5) >> 4) & 0x3fff) + 1;
  if (((d[8] >> 2) & 0x0f) != 13)       /* SFREQ 13 = 48 kHz */
    return 0;
  switch (blocks) {
    case 16:
      type = 0x0B;
      break;
    case 32:
      type = 0x0C;
      break;
    case 64:
      type = 0x0D;
      break;
    default:
      return 0;
  }
  period = (gint) blocks << 7;
  if ((gsize) core > size || core + (core & 1) > period - 8)
    return 0;
  if (out == NULL)
    return period;
  if (out_size != period)
    return 0;

  length = ((gsize) core < size ? core : (core + 1) & ~1) << 3;

  memset (out, 0, period);
  GST_WRITE_UINT16_LE (out, 0xF872);
  GST_WRITE_UINT16_LE (out + 2, 0x4E1F);
  GST_WRITE_UINT16_LE (out + 4, type);
  GST_WRITE_UINT16_LE (out + 6, length);
  for (i = 0; i + 1 < core; i += 2) {
    out[8 + i] = d[i + 1];
    out[8 + i + 1] = d[i];
  }
  if (core & 1)
    out[8 + core] = d[core - 1];
  return period;
}

/* ---------------------------------------------------------------------------
 * Output decision
 * ------------------------------------------------------------------------- */

/* 1 when the live watcher reports a receiver output with LG's Digital Sound
 * Output on passThrough. File format (the boot script's watcher):
 * "<watcher pid> <soundOutput> <soundOutputDigital>". */
static gboolean
dtsc1_receiver_live (gchar * why, gsize why_len)
{
  long pid;
  char output[64], mode[64], proc[64];
  int fields;
  FILE *file = fopen (DTSC1_SOUND_OUTPUT, "r");

  if (!file) {
    g_strlcpy (why, "no sound-output watcher", why_len);
    return FALSE;
  }
  fields = fscanf (file, "%ld %63s %63s", &pid, output, mode);
  fclose (file);
  if (fields != 3 || pid <= 0) {
    g_strlcpy (why, "unreadable watcher file", why_len);
    return FALSE;
  }
  snprintf (proc, sizeof proc, "/proc/%ld", pid);
  if (access (proc, F_OK) != 0) {
    g_strlcpy (why, "watcher not running", why_len);
    return FALSE;
  }
  if (strcmp (output, "external_optical") && strcmp (output, "external_arc")) {
    g_snprintf (why, why_len, "sound output is %s", output);
    return FALSE;
  }
  if (strcmp (mode, "passThrough")) {
    g_snprintf (why, why_len, "Digital Sound Output is %s", mode);
    return FALSE;
  }
  g_snprintf (why, why_len, "%s, %s", output, mode);
  return TRUE;
}

static void
dtsc1_record (GstDtsDec * dts, const gchar * what, const gchar * why)
{
  FILE *f;

  GST_INFO_OBJECT (dts, "%s (%s)", what, why);
  if (access (DTSC1_RUN_DIR, W_OK) != 0)
    return;
  f = fopen (DTSC1_LAST, "w");
  if (!f)
    return;
  fprintf (f, "%" G_GINT64_FORMAT " %s %s\n", g_get_real_time () / G_USEC_PER_SEC,
      what, why);
  fclose (f);
}

static DtsDecOutput
dtsc1_effective_mode (GstDtsDec * dts)
{
  const gchar *env = g_getenv ("DTSDEC_OUTPUT");

  if (dts->output != DTSDEC_OUTPUT_AUTO)
    return dts->output;
  if (env && !strcmp (env, "passthrough"))
    return DTSDEC_OUTPUT_PASSTHROUGH;
  if (env && !strcmp (env, "decode"))
    return DTSDEC_OUTPUT_DECODE;
  return DTSDEC_OUTPUT_AUTO;
}

static gboolean
gst_dtsdec_set_pt_caps (GstDtsDec * dts)
{
  GstAudioInfo info;

  gst_audio_info_init (&info);
  gst_audio_info_set_format (&info, GST_AUDIO_FORMAT_S16LE, 48000, 2, NULL);
  return gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (dts), &info);
}

/* Decide for this stream; the first frame decides the format. */
static void
gst_dtsdec_decide (GstDtsDec * dts, const guint8 * d, gsize size)
{
  gchar why[160];
  DtsDecOutput mode = dtsc1_effective_mode (dts);
  gint period;

  dts->decided = TRUE;
  dts->passthrough = FALSE;
  dts->pt_period = 0;

  if (mode == DTSDEC_OUTPUT_DECODE) {
    dtsc1_record (dts, "decode", "decode forced");
    return;
  }
  if (mode == DTSDEC_OUTPUT_AUTO) {
    if (access (DTSC1_PT_FLAG, F_OK) != 0) {
      dtsc1_record (dts, "decode", "passthrough not enabled");
      return;
    }
    if (!dtsc1_receiver_live (why, sizeof why)) {
      dtsc1_record (dts, "decode", why);
      return;
    }
  } else {
    g_strlcpy (why, "passthrough forced", sizeof why);
  }

  period = dts_iec61937_pack (d, size, NULL, 0);
  if (period == 0) {
    dtsc1_record (dts, "decode", "not a 16-bit 48 kHz DTS core");
    return;
  }
  if (!gst_dtsdec_set_pt_caps (dts)) {
    dtsc1_record (dts, "decode", "S16LE 48 kHz stereo caps refused");
    return;
  }
  dts->passthrough = TRUE;
  dts->pt_period = period;
  dts->pt_auto = (mode == DTSDEC_OUTPUT_AUTO);
  dts->pt_checked = g_get_monotonic_time ();
  dts->dec_rate = 0;
  dtsc1_record (dts, "passthrough", why);
}

/* ---------------------------------------------------------------------------
 * Decode path: FFmpeg dca -> stereo S16LE downmix
 * ------------------------------------------------------------------------- */

/* Left/right weights per FFmpeg channel bit (ITU-style -3 dB centre and
 * surrounds, LFE dropped). The sum is normalized so a full-scale input cannot
 * clip. */
static void
dtsc1_weights (guint64 bit, gfloat * wl, gfloat * wr)
{
  const gfloat h = 0.70710678f;

  *wl = *wr = 0.0f;
  switch (bit) {
    case AV_CH_FRONT_LEFT:
    case AV_CH_FRONT_LEFT_OF_CENTER:
    case AV_CH_WIDE_LEFT:
      *wl = 1.0f;
      break;
    case AV_CH_FRONT_RIGHT:
    case AV_CH_FRONT_RIGHT_OF_CENTER:
    case AV_CH_WIDE_RIGHT:
      *wr = 1.0f;
      break;
    case AV_CH_FRONT_CENTER:
    case AV_CH_TOP_CENTER:
    case AV_CH_TOP_FRONT_CENTER:
      *wl = *wr = h;
      break;
    case AV_CH_BACK_LEFT:
    case AV_CH_SIDE_LEFT:
    case AV_CH_TOP_FRONT_LEFT:
    case AV_CH_TOP_BACK_LEFT:
    case AV_CH_SURROUND_DIRECT_LEFT:
      *wl = h;
      break;
    case AV_CH_BACK_RIGHT:
    case AV_CH_SIDE_RIGHT:
    case AV_CH_TOP_FRONT_RIGHT:
    case AV_CH_TOP_BACK_RIGHT:
    case AV_CH_SURROUND_DIRECT_RIGHT:
      *wr = h;
      break;
    case AV_CH_BACK_CENTER:
    case AV_CH_TOP_BACK_CENTER:
      *wl = *wr = 0.5f;
      break;
    default:                   /* LFE, LFE2 */
      break;
  }
}

static gfloat
dtsc1_sample (const AVFrame * f, gint ch, gint n)
{
  switch (f->format) {
    case AV_SAMPLE_FMT_FLTP:
      return ((const float *) f->extended_data[ch])[n];
    case AV_SAMPLE_FMT_S32P:
      return (gfloat) (((const gint32 *) f->extended_data[ch])[n] /
          2147483648.0);
    case AV_SAMPLE_FMT_S16P:
      return ((const gint16 *) f->extended_data[ch])[n] / 32768.0f;
    default:
      return 0.0f;
  }
}

static GstFlowReturn
gst_dtsdec_output_decoded (GstDtsDec * dts, const AVFrame * f)
{
  GstAudioDecoder *bdec = GST_AUDIO_DECODER (dts);
  gfloat wl[64], wr[64], suml = 0.0f, sumr = 0.0f, norm;
  guint64 layout = f->channel_layout, bit;
  gint chans = f->channels, nb = f->nb_samples, c, n;
  GstBuffer *outbuf;
  GstMapInfo map;
  gint16 *out;

  if (f->format != AV_SAMPLE_FMT_FLTP && f->format != AV_SAMPLE_FMT_S32P
      && f->format != AV_SAMPLE_FMT_S16P) {
    GST_ELEMENT_ERROR (dts, STREAM, DECODE, (NULL),
        ("unexpected FFmpeg sample format %d", f->format));
    return GST_FLOW_ERROR;
  }
  if (chans <= 0 || chans > 64 || f->sample_rate <= 0)
    return GST_FLOW_OK;
  if (layout == 0)
    layout = av_get_default_channel_layout (chans);

  /* planes are in ascending channel-bit order */
  for (c = 0, bit = 1; c < chans && bit; bit <<= 1) {
    if (!(layout & bit))
      continue;
    dtsc1_weights (bit, &wl[c], &wr[c]);
    suml += wl[c];
    sumr += wr[c];
    c++;
  }
  for (; c < chans; c++)
    wl[c] = wr[c] = 0.0f;
  if (chans == 1) {
    wl[0] = wr[0] = 1.0f;
    suml = sumr = 1.0f;
  }
  norm = MAX (suml, sumr);
  if (norm <= 0.0f)
    norm = 1.0f;

  if (dts->dec_rate != f->sample_rate) {
    GstAudioInfo info;

    gst_audio_info_init (&info);
    gst_audio_info_set_format (&info, GST_AUDIO_FORMAT_S16LE, f->sample_rate,
        2, NULL);
    if (!gst_audio_decoder_set_output_format (bdec, &info)) {
      GST_ELEMENT_ERROR (dts, CORE, NEGOTIATION, (NULL), (NULL));
      return GST_FLOW_NOT_NEGOTIATED;
    }
    dts->dec_rate = f->sample_rate;
  }

  if (!dts->tagged) {
    GstTagList *tags = gst_tag_list_new (GST_TAG_AUDIO_CODEC, "DTS", NULL);

    gst_audio_decoder_merge_tags (bdec, tags, GST_TAG_MERGE_REPLACE);
    gst_tag_list_unref (tags);
    dts->tagged = TRUE;
  }

  outbuf = gst_audio_decoder_allocate_output_buffer (bdec, (gsize) nb * 4);
  if (!outbuf || !gst_buffer_map (outbuf, &map, GST_MAP_WRITE)) {
    if (outbuf)
      gst_buffer_unref (outbuf);
    return GST_FLOW_ERROR;
  }
  out = (gint16 *) map.data;
  for (n = 0; n < nb; n++) {
    gfloat l = 0.0f, r = 0.0f;

    for (c = 0; c < chans; c++) {
      gfloat s = dtsc1_sample (f, c, n);
      l += s * wl[c];
      r += s * wr[c];
    }
    l = CLAMP (l / norm * 32768.0f, -32768.0f, 32767.0f);
    r = CLAMP (r / norm * 32768.0f, -32768.0f, 32767.0f);
    GST_WRITE_UINT16_LE (&out[2 * n], (guint16) (gint16) lrintf (l));
    GST_WRITE_UINT16_LE (&out[2 * n + 1], (guint16) (gint16) lrintf (r));
  }
  gst_buffer_unmap (outbuf, &map);
  return gst_audio_decoder_finish_frame (bdec, outbuf, 1);
}

/* Copy one frame into the zero-padded packet buffer FFmpeg needs (it reads
 * up to AV_INPUT_BUFFER_PADDING_SIZE past the end). Done while the input is
 * still mapped: finish_frame() releases the input buffer. */
static gboolean
gst_dtsdec_stage_packet (GstDtsDec * dts, const guint8 * d, gsize size)
{
  gsize need = size + AV_INPUT_BUFFER_PADDING_SIZE;

  if (dts->pktbuf_size < need) {
    av_freep (&dts->pktbuf);
    dts->pktbuf = av_malloc (need);
    dts->pktbuf_size = dts->pktbuf ? need : 0;
  }
  if (dts->pktbuf == NULL)
    return FALSE;
  memcpy (dts->pktbuf, d, size);
  memset (dts->pktbuf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  dts->pkt->data = dts->pktbuf;
  dts->pkt->size = (int) size;
  return TRUE;
}

/* Decode the staged packet. The input buffer must already be unmapped. */
static GstFlowReturn
gst_dtsdec_decode (GstDtsDec * dts)
{
  GstAudioDecoder *bdec = GST_AUDIO_DECODER (dts);
  GstFlowReturn result = GST_FLOW_OK;
  gint ret;

  ret = avcodec_send_packet (dts->avctx, dts->pkt);
  av_packet_unref (dts->pkt);
  if (ret < 0) {
    GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
        ("dts_frame error %d", ret), result);
    return result;
  }
  ret = avcodec_receive_frame (dts->avctx, dts->frame);
  if (ret == AVERROR (EAGAIN))
    return gst_audio_decoder_finish_frame (bdec, NULL, 1);
  if (ret < 0) {
    GST_AUDIO_DECODER_ERROR (dts, 1, STREAM, DECODE, (NULL),
        ("error decoding frame %d", ret), result);
    return result;
  }
  result = gst_dtsdec_output_decoded (dts, dts->frame);
  av_frame_unref (dts->frame);
  return result;
}

/* ---------------------------------------------------------------------------
 * GstAudioDecoder vfuncs
 * ------------------------------------------------------------------------- */

static gboolean
gst_dtsdec_start (GstAudioDecoder * dec)
{
  GstDtsDec *dts = GST_DTSDEC (dec);
  const AVCodec *codec = avcodec_find_decoder (AV_CODEC_ID_DTS);

  if (codec == NULL) {
    GST_ELEMENT_ERROR (dts, LIBRARY, INIT, (NULL),
        ("FFmpeg was built without the dca decoder"));
    return FALSE;
  }
  dts->avctx = avcodec_alloc_context3 (codec);
  dts->frame = av_frame_alloc ();
  dts->pkt = av_packet_alloc ();
  if (!dts->avctx || !dts->frame || !dts->pkt
      || avcodec_open2 (dts->avctx, codec, NULL) < 0) {
    GST_ELEMENT_ERROR (dts, LIBRARY, INIT, (NULL),
        ("could not open FFmpeg's dca decoder"));
    return FALSE;
  }
  dts->decided = FALSE;
  dts->passthrough = FALSE;
  dts->pt_period = 0;
  dts->dec_rate = 0;
  dts->tagged = FALSE;
  gst_audio_decoder_set_estimate_rate (dec, TRUE);
  return TRUE;
}

static gboolean
gst_dtsdec_stop (GstAudioDecoder * dec)
{
  GstDtsDec *dts = GST_DTSDEC (dec);

  g_free (dts->stream_id);
  dts->stream_id = NULL;
  avcodec_free_context (&dts->avctx);
  av_frame_free (&dts->frame);
  av_packet_free (&dts->pkt);
  av_freep (&dts->pktbuf);
  dts->pktbuf_size = 0;
  return TRUE;
}

static gboolean
gst_dtsdec_sink_event (GstAudioDecoder * dec, GstEvent * event)
{
  GstDtsDec *dts = GST_DTSDEC (dec);

  /* A new track (or a new file in the same pipeline) decides again: the user
   * may have switched the sound output in between. */
  if (GST_EVENT_TYPE (event) == GST_EVENT_STREAM_START) {
    const gchar *id = NULL;

    gst_event_parse_stream_start (event, &id);
    if (g_strcmp0 (id, dts->stream_id)) {
      dts->decided = FALSE;
      g_free (dts->stream_id);
      dts->stream_id = g_strdup (id);
    }
  }
  return GST_AUDIO_DECODER_CLASS (gst_dtsdec_parent_class)->sink_event (dec,
      event);
}

static GstFlowReturn
gst_dtsdec_parse (GstAudioDecoder * bdec, GstAdapter * adapter,
    gint * _offset, gint * len)
{
  guint8 *data;
  gint av, size, length = 0;
  GstFlowReturn result = GST_FLOW_EOS;

  size = av = gst_adapter_available (adapter);
  data = (guint8 *) gst_adapter_map (adapter, av);
  while (size >= 4) {
    length = dts_frame_size (data, size);
    if (length == 0) {
      data++;
      size--;
    } else if (length > 0 && length <= size) {
      result = GST_FLOW_OK;
      break;
    } else {
      break;
    }
  }
  gst_adapter_unmap (adapter);
  *_offset = av - size;
  *len = length;
  return result;
}

static GstFlowReturn
gst_dtsdec_handle_frame (GstAudioDecoder * bdec, GstBuffer * buffer)
{
  GstDtsDec *dts = GST_DTSDEC (bdec);
  GstMapInfo map;

  if (G_UNLIKELY (!buffer))
    return GST_FLOW_OK;

  gst_buffer_map (buffer, &map, GST_MAP_READ);
  if (!dts->decided)
    gst_dtsdec_decide (dts, map.data, map.size);

  /* Re-check the live sound output once a second: switching to TV speakers
   * mid-play must not leave the bitstream on them. On a clock, because a frame
   * holds 512, 1024 or 2048 samples. */
  if (dts->passthrough && dts->pt_auto
      && g_get_monotonic_time () - dts->pt_checked >= G_USEC_PER_SEC) {
    gchar why[160];

    dts->pt_checked = g_get_monotonic_time ();
    if (access (DTSC1_PT_FLAG, F_OK) != 0) {
      dts->passthrough = FALSE;
      dtsc1_record (dts, "decode", "passthrough disabled mid-play");
    } else if (!dtsc1_receiver_live (why, sizeof why)) {
      dts->passthrough = FALSE;
      dtsc1_record (dts, "decode", why);
    }
  }

  if (dts->passthrough) {
    gint period = dts_iec61937_pack (map.data, map.size, NULL, 0);

    if (period == dts->pt_period) {
      GstBuffer *outbuf =
          gst_audio_decoder_allocate_output_buffer (bdec, (gsize) period);
      GstMapInfo omap;

      if (outbuf && gst_buffer_map (outbuf, &omap, GST_MAP_WRITE)) {
        dts_iec61937_pack (map.data, map.size, omap.data, period);
        gst_buffer_unmap (outbuf, &omap);
        gst_buffer_unmap (buffer, &map);
        return gst_audio_decoder_finish_frame (bdec, outbuf, 1);
      }
      if (outbuf)
        gst_buffer_unref (outbuf);
    }
    /* A frame the burst cannot carry (period change, non-48 kHz core): this
     * stream decodes from here on. The receiver drops out briefly. */
    dts->passthrough = FALSE;
    dtsc1_record (dts, "decode", "stream changed mid-play, decoding instead");
  }

  if (!gst_dtsdec_stage_packet (dts, map.data, map.size)) {
    gst_buffer_unmap (buffer, &map);
    return GST_FLOW_ERROR;
  }
  gst_buffer_unmap (buffer, &map);
  return gst_dtsdec_decode (dts);
}

/* GstAudioDecoder's default sink getcaps copies the src side's rate/channels
 * onto the sink caps (proxy_getcaps). Our output is always 2 channels, so that
 * would turn the sink caps into "audio/x-dts, channels=2" and refuse every
 * 5.1 track before a byte flows (seen under qemu: "Delayed linking failed").
 * The input does not depend on the output format: answer the template. */
static GstCaps *
gst_dtsdec_getcaps (GstAudioDecoder * bdec, GstCaps * filter)
{
  GstCaps *tmpl = gst_pad_get_pad_template_caps (GST_AUDIO_DECODER_SINK_PAD (bdec));
  GstCaps *res;

  if (!filter)
    return tmpl;
  res = gst_caps_intersect_full (filter, tmpl, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (tmpl);
  return res;
}

static gboolean
gst_dtsdec_set_format (GstAudioDecoder * bdec, GstCaps * caps)
{
  GstDtsDec *dts = GST_DTSDEC (bdec);
  GstStructure *s = gst_caps_get_structure (caps, 0);

  dts->dvdmode = s && gst_structure_has_name (s, "audio/x-private1-dts");
  return TRUE;
}

/* DVD LPCM-style first_access header, verbatim logic from upstream dtsdec. */
static GstFlowReturn
gst_dtsdec_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstDtsDec *dts = GST_DTSDEC (parent);
  GstFlowReturn ret = GST_FLOW_OK;
  guint8 hdr[2];
  gsize size;
  gint first_access, offset = 2, len;
  GstBuffer *subbuf;

  if (!dts->dvdmode)
    return dts->base_chain (pad, parent, buf);

  size = gst_buffer_get_size (buf);
  if (size < 2) {
    GST_ELEMENT_ERROR (dts, STREAM, DECODE, (NULL),
        ("Insufficient data in buffer. Can't determine first_access"));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
  gst_buffer_extract (buf, 0, hdr, 2);
  first_access = (hdr[0] << 8) | hdr[1];

  if (first_access > 1) {
    len = first_access - 1;
    if (len <= 0 || (gsize) (offset + len) > size) {
      GST_ELEMENT_ERROR (dts, STREAM, DECODE, (NULL),
          ("Bad first_access parameter (%d) in buffer", first_access));
      gst_buffer_unref (buf);
      return GST_FLOW_ERROR;
    }
    subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
    GST_BUFFER_PTS (subbuf) = GST_CLOCK_TIME_NONE;
    ret = dts->base_chain (pad, parent, subbuf);
    if (ret != GST_FLOW_OK) {
      gst_buffer_unref (buf);
      return ret;
    }
    offset += len;
    len = (gint) size - offset;
    if (len > 0) {
      subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset, len);
      GST_BUFFER_PTS (subbuf) = GST_BUFFER_PTS (buf);
      ret = dts->base_chain (pad, parent, subbuf);
    }
  } else {
    subbuf = gst_buffer_copy_region (buf, GST_BUFFER_COPY_ALL, offset,
        size - offset);
    GST_BUFFER_PTS (subbuf) = GST_BUFFER_PTS (buf);
    ret = dts->base_chain (pad, parent, subbuf);
  }
  gst_buffer_unref (buf);
  return ret;
}

static void
gst_dtsdec_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDEC (object);

  switch (prop_id) {
    case PROP_OUTPUT:
      dts->output = (DtsDecOutput) g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsdec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDtsDec *dts = GST_DTSDEC (object);

  switch (prop_id) {
    case PROP_OUTPUT:
      g_value_set_enum (value, dts->output);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dtsdec_class_init (GstDtsDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAudioDecoderClass *bdec_class = GST_AUDIO_DECODER_CLASS (klass);

  gobject_class->set_property = gst_dtsdec_set_property;
  gobject_class->get_property = gst_dtsdec_get_property;

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_set_static_metadata (element_class, "DTS audio decoder",
      "Codec/Decoder/Audio",
      "DTS core to IEC 61937 for an S/PDIF receiver, or stereo decode",
      "Jan Schmidt <thaytan@noraisin.net>, "
      "Ronald Bultje <rbultje@ronald.bitfreak.net>");

  bdec_class->start = GST_DEBUG_FUNCPTR (gst_dtsdec_start);
  bdec_class->stop = GST_DEBUG_FUNCPTR (gst_dtsdec_stop);
  bdec_class->set_format = GST_DEBUG_FUNCPTR (gst_dtsdec_set_format);
  bdec_class->parse = GST_DEBUG_FUNCPTR (gst_dtsdec_parse);
  bdec_class->handle_frame = GST_DEBUG_FUNCPTR (gst_dtsdec_handle_frame);
  bdec_class->sink_event = GST_DEBUG_FUNCPTR (gst_dtsdec_sink_event);
  bdec_class->getcaps = GST_DEBUG_FUNCPTR (gst_dtsdec_getcaps);

  g_object_class_install_property (gobject_class, PROP_OUTPUT,
      g_param_spec_enum ("output", "Output",
          "auto: IEC 61937 passthrough only when opted in and an optical/ARC "
          "receiver is the live sound output; otherwise stereo decode",
          gst_dtsdec_output_get_type (), DTSDEC_OUTPUT_AUTO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_dtsdec_init (GstDtsDec * dts)
{
  dts->base_chain = GST_PAD_CHAINFUNC (GST_AUDIO_DECODER_SINK_PAD (dts));
  gst_pad_set_chain_function (GST_AUDIO_DECODER_SINK_PAD (dts),
      GST_DEBUG_FUNCPTR (gst_dtsdec_chain));
  gst_audio_decoder_set_use_default_pad_acceptcaps (GST_AUDIO_DECODER_CAST
      (dts), TRUE);
  dts->output = DTSDEC_OUTPUT_AUTO;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (dtsdec_debug, "dtsdec", 0,
      "DTS decoder / IEC 61937 packer (webOS 6)");
  av_log_set_level (AV_LOG_ERROR);
  return gst_element_register (plugin, "dtsdec", GST_RANK_PRIMARY,
      GST_TYPE_DTSDEC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dtsdec,
    "DTS passthrough/decoder for webOS 6",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
