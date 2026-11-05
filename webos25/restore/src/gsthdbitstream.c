/* gsthdbitstream.c — lossless bitstream forwarder for an eARC receiver (webOS 25)
 *
 * A decoder-shaped element that does not decode: it packs DTS / DTS-HD frames
 * as IEC 61937 type 17 bursts and TrueHD / MLP access units as MAT (type 22)
 * bursts (iec61937-hbr.c) and emits them as ES-flagged 8-channel 16-bit
 * 192 kHz buffers for LG's own eARC bypass route (lgroute.c, approach A: no
 * kernel module). The receiver then decodes DTS-HD MA, DTS:X, TrueHD and
 * Atmos itself.
 *
 * adecswitch plugs it instead of dtsdec / avdec_truehd only while
 * lgroute_wanted() says the user opted in and a receiver is reported. Anything
 * that then goes wrong -- no LG audiosink to arm, a frame the packer cannot
 * carry -- posts an `hdbitstream-fallback` element message and drops the rest;
 * adecswitch answers the message by plugging the real decoder, so the stream
 * decodes as if this element had never existed. The first bursts go out as
 * silence of the same length, so a receiver still in PCM mode gets no noise
 * while LG connects the route and the eARC output switches to HBR.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiodecoder.h>

#include "hdparse.h"
#include "lgroute.h"
#include "iec61937-hbr.h"

/* Shared with lgroute.c, so GST_DEBUG=hdbitstream:5 shows the route too. */
GST_DEBUG_CATEGORY (hdbitstream_debug);
#define GST_CAT_DEFAULT hdbitstream_debug

/* Silence before the first real burst: stream time, not wall time, so a
 * paused preroll does not use it up. */
#define HDBITSTREAM_LEAD_SILENCE_FRAMES (IEC61937_HBR_RATE / 2)
/* A DTS-HD stream can open with extension-only frames; after this many
 * without a core it is not one the type 17 packer can carry. */
#define HDBITSTREAM_MAX_LEADING_EXSS 8
/* Bytes the sync scan may skip without finding a frame before the stream is
 * handed to the decoder: 14-bit and little-endian DTS have no frame this
 * parser knows, and GstAudioDecoder would otherwise error the pipeline after
 * GST_AUDIO_DECODER_MAX_SYNC (160 KiB, gstaudiodecoder.c) instead. */
#define HDBITSTREAM_MAX_SCAN (64 * 1024)

typedef struct
{
  GstAudioDecoder parent;

  LgRoute route;
  gboolean truehd;              /* sink caps: TrueHD/MLP, else DTS */
  gboolean started;             /* route armed and carrier caps negotiated */
  gboolean abandoned;           /* fell back: everything is dropped now */
  guint64 gated;                /* carrier frames of silence sent so far */
  guint pending;                /* input frames queued for the next burst */
  guint scanned;                /* bytes skipped since the last frame */
  guint leading_exss;           /* EXSS-only frames dropped before a core */
  gboolean sent;                /* a burst with payload has gone out */
  gboolean mlp_synced;          /* a major sync was seen; lengths are trusted */
  Iec61937MatPacker *mat;
  guint8 *burst;
} GstHdBitstream;

typedef struct
{
  GstAudioDecoderClass parent_class;
} GstHdBitstreamClass;

#define GST_TYPE_HDBITSTREAM (gst_hdbitstream_get_type ())
#define GST_HDBITSTREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_HDBITSTREAM, GstHdBitstream))

G_GNUC_INTERNAL GType gst_hdbitstream_get_type (void);
G_DEFINE_TYPE (GstHdBitstream, gst_hdbitstream, GST_TYPE_AUDIO_DECODER);

/* audio/x-private1-dts is not taken: its per-packet substream header would
 * land inside frames that span packets. dtsdec keeps decoding it. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; audio/x-unknown, codec-id = (string) A_DTS; "
        "audio/x-true-hd; audio/x-mlp"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, format = (string) S16LE, "
        "channels = (int) 8, rate = (int) 192000, layout = (string) interleaved"));

static gboolean
gst_hdbitstream_start (GstAudioDecoder * dec)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);

  lgroute_init (&self->route, GST_ELEMENT (self));
  self->started = self->abandoned = self->sent = self->mlp_synced = FALSE;
  self->gated = 0;
  self->pending = self->scanned = self->leading_exss = 0;
  self->mat = g_malloc (sizeof (*self->mat));
  iec61937_mat_init (self->mat);
  self->burst = g_malloc (IEC61937_DTSHD_MAX_BURST);
  return TRUE;
}

static gboolean
gst_hdbitstream_stop (GstAudioDecoder * dec)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);

  lgroute_disarm (&self->route);
  g_free (self->mat);
  g_free (self->burst);
  self->mat = NULL;
  self->burst = NULL;
  return TRUE;
}

static gboolean
gst_hdbitstream_set_format (GstAudioDecoder * dec, GstCaps * caps)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);
  const gchar *name = gst_structure_get_name (gst_caps_get_structure (caps, 0));
  GstCaps *carrier;
  gboolean ok;

  self->truehd = !strcmp (name, "audio/x-true-hd") || !strcmp (name, "audio/x-mlp");
  /* New caps start a new stream for the packers: a MAT frame half filled by
   * the old one, or its queued frames, must not carry into the next. */
  if (self->mat != NULL)
    iec61937_mat_init (self->mat);
  self->pending = self->scanned = self->leading_exss = 0;
  self->mlp_synced = FALSE;
  /* The ES fields ride on the caps: lgaudiosink reads them into its first
   * event (render_element.c:497-515), GstAudioDecoder only needs the PCM
   * part to be parsable (gstaudiodecoder.c:860). */
  carrier = lgroute_carrier_caps ();
  ok = gst_audio_decoder_set_output_caps (dec, carrier);
  gst_caps_unref (carrier);
  return ok;
}

/* The compressed stream's rate and channel count have nothing to do with the
 * carrier's, so downstream caps are not proxied upstream. */
static GstCaps *
gst_hdbitstream_getcaps (GstAudioDecoder * dec, GstCaps * filter)
{
  GstCaps *caps = gst_pad_get_pad_template_caps (GST_AUDIO_DECODER_SINK_PAD (dec));

  if (filter != NULL) {
    GstCaps *r = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);

    gst_caps_unref (caps);
    return r;
  }
  return caps;
}

/* Disarms first, so the decoder adecswitch plugs next negotiates with
 * pcm-multi-ch already back where it was, then tells adecswitch. */
static void
gst_hdbitstream_abandon (GstHdBitstream * self, const gchar * reason)
{
  lgroute_log (GST_ELEMENT (self), "falling back to decoding: %s", reason);
  lgroute_disarm (&self->route);
  self->abandoned = TRUE;
  gst_element_post_message (GST_ELEMENT (self),
      gst_message_new_element (GST_OBJECT (self),
          gst_structure_new ("hdbitstream-fallback", "reason", G_TYPE_STRING,
              reason, NULL)));
}

static GstFlowReturn
gst_hdbitstream_fallback (GstHdBitstream * self, const gchar * reason)
{
  gst_hdbitstream_abandon (self, reason);
  return gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self), NULL,
      MAX (self->pending, 1));
}

static GstFlowReturn
gst_hdbitstream_parse (GstAudioDecoder * dec, GstAdapter * adapter,
    gint * offset, gint * length)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);
  gint avail = gst_adapter_available (adapter), at, size = -1;
  const guint8 *data;
  gboolean eos = FALSE;

  if (self->abandoned) {
    /* Taken as one frame that handle_frame drops: skipping it instead would
     * count towards the base class's parse-failure limit. */
    *offset = 0;
    *length = avail;
    return GST_FLOW_OK;
  }
  data = gst_adapter_map (adapter, avail);
  for (at = 0; at + 8 <= avail; at++) {
    /* A TrueHD length is trusted only right after a frame; a scan, or the
     * first frame after a flush, needs a major sync. DTS re-syncs itself. */
    size = self->truehd
        ? hdparse_mlp_au (data + at, avail - at, !self->mlp_synced || at > 0)
        : hdparse_dts_frame (data + at, avail - at);
    if (size != 0)
      break;
  }
  /* The last frame of a core-only DTS stream: at the end there are no 4
   * bytes after the core to say an EXSS does not follow it. */
  gst_audio_decoder_get_parse_state (dec, NULL, &eos);
  if (size == -1 && eos && !self->truehd) {
    size = hdparse_dts_core (data + at, avail - at);
    if (size <= 0 || at + size > avail)
      size = -1;
  }
  if (self->truehd && size >= 0)
    self->mlp_synced = size > 0;
  gst_adapter_unmap (adapter);
  if (size > 0 && at + size <= avail) {
    self->scanned = 0;
    *offset = at;
    *length = size;
    return GST_FLOW_OK;
  }
  *offset = size > 0 || size == -1 ? at : MAX (avail - 7, 0);
  *length = 0;
  self->scanned += *offset;
  if (self->scanned > HDBITSTREAM_MAX_SCAN) {
    gst_hdbitstream_abandon (self, self->truehd
        ? "no TrueHD major sync in 64 KiB"
        : "no DTS frame in 64 KiB (14-bit or little-endian DTS?)");
    /* Nothing more is skipped: the base class counts skipped bytes towards
     * its own 160 KiB limit, and adecswitch retargets the data now. */
    *offset = 0;
  }
  return GST_FLOW_EOS;
}

static GstFlowReturn
gst_hdbitstream_handle_frame (GstAudioDecoder * dec, GstBuffer * input)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);
  GstMapInfo map;
  GstBuffer *out;
  const gchar *why = NULL;
  gint size;

  if (input == NULL)
    return GST_FLOW_OK;
  if (self->abandoned)
    return gst_audio_decoder_finish_frame (dec, NULL, 1);

  if (!self->started) {
    /* The probes must be on the sink before the carrier caps reach it: the
     * sink builds its renderer from them (render_element.c:570-573). */
    if (!lgroute_arm (&self->route, &why))
      return gst_hdbitstream_fallback (self, why);
    if (!gst_audio_decoder_negotiate (dec))
      return gst_hdbitstream_fallback (self, "the carrier caps were refused");
    self->started = TRUE;
  }

  if (!gst_buffer_map (input, &map, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (self->truehd) {
    gint r = iec61937_mat_push (self->mat, map.data, map.size, self->burst,
        IEC61937_DTSHD_MAX_BURST);

    gst_buffer_unmap (input, &map);
    if (r == IEC61937_HBR_ENOSYNC)      /* before the first major sync */
      return gst_audio_decoder_finish_frame (dec, NULL, 1);
    if (r < 0)
      return gst_hdbitstream_fallback (self, "an access unit the MAT packer refuses");
    self->pending++;
    if (r == 0)                 /* queued; the MAT frame is not full yet */
      return GST_FLOW_OK;
    size = IEC61937_MAT_BURST_BYTES;
  } else {
    gint r = iec61937_dtshd_pack (map.data, map.size, self->burst,
        IEC61937_DTSHD_MAX_BURST);

    if (r == IEC61937_HBR_EINVAL && map.size >= 4
        && hdparse_be32 (map.data) == HDPARSE_DTS_SYNC_EXSS && !self->sent
        && ++self->leading_exss <= HDBITSTREAM_MAX_LEADING_EXSS) {
      gst_buffer_unmap (input, &map);
      return gst_audio_decoder_finish_frame (dec, NULL, 1);
    }
    gst_buffer_unmap (input, &map);
    if (r < 0)
      return gst_hdbitstream_fallback (self,
          r == IEC61937_HBR_ERATE ? "a DTS rate the 192 kHz carrier cannot take"
          : r == IEC61937_HBR_E2BIG ? "a DTS frame larger than its burst"
          : "a DTS frame without a 16-bit big-endian core");
    self->pending = 1;
    size = r;
  }

  if (self->gated < HDBITSTREAM_LEAD_SILENCE_FRAMES) {
    memset (self->burst, 0, size);
    self->gated += IEC61937_HBR_FRAMES (size);
  } else if (!self->sent) {
    lgroute_log (GST_ELEMENT (self), "sending %s bursts after %"
        G_GUINT64_FORMAT " ms of silence", self->truehd ? "MAT" : "DTS-HD",
        self->gated * 1000 / IEC61937_HBR_RATE);
    self->sent = TRUE;
  }
  out = gst_audio_decoder_allocate_output_buffer (dec, size);
  gst_buffer_fill (out, 0, self->burst, size);
  /* lgaudiosink feeds ES-flagged buffers to its bypass appsrc only
   * (render_element.c:916-917, lgao.c:2070-2073): never the speakers. */
  GST_BUFFER_FLAG_SET (out, LGROUTE_BUFFER_FLAG_ES);
  size = self->pending;
  self->pending = 0;
  return gst_audio_decoder_finish_frame (dec, out, size);
}

/* Hard flush (seek): the base class drops the queued frames and so must the
 * packer. The route stays: LG applied the flag when it created the renderer. */
static void
gst_hdbitstream_flush (GstAudioDecoder * dec, gboolean hard)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);

  if (hard && self->mat != NULL) {
    iec61937_mat_init (self->mat);
    self->pending = 0;
    self->mlp_synced = FALSE;
  }
}

static gboolean
gst_hdbitstream_sink_event (GstAudioDecoder * dec, GstEvent * event)
{
  GstHdBitstream *self = GST_HDBITSTREAM (dec);
  gboolean eos = GST_EVENT_TYPE (event) == GST_EVENT_EOS;
  gboolean ret = GST_AUDIO_DECODER_CLASS (gst_hdbitstream_parent_class)
      ->sink_event (dec, event);

  if (eos)
    lgroute_disarm (&self->route);
  return ret;
}

static void
gst_hdbitstream_class_init (GstHdBitstreamClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAudioDecoderClass *dec_class = GST_AUDIO_DECODER_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class,
      "Lossless bitstream forwarder", "Codec/Decoder/Audio",
      "Sends DTS-HD and TrueHD to an eARC receiver as IEC 61937 bursts "
      "through LG's own eARC bypass route",
      "dts_restore_plus");

  dec_class->start = GST_DEBUG_FUNCPTR (gst_hdbitstream_start);
  dec_class->stop = GST_DEBUG_FUNCPTR (gst_hdbitstream_stop);
  dec_class->set_format = GST_DEBUG_FUNCPTR (gst_hdbitstream_set_format);
  dec_class->getcaps = GST_DEBUG_FUNCPTR (gst_hdbitstream_getcaps);
  dec_class->parse = GST_DEBUG_FUNCPTR (gst_hdbitstream_parse);
  dec_class->handle_frame = GST_DEBUG_FUNCPTR (gst_hdbitstream_handle_frame);
  dec_class->flush = GST_DEBUG_FUNCPTR (gst_hdbitstream_flush);
  dec_class->sink_event = GST_DEBUG_FUNCPTR (gst_hdbitstream_sink_event);
}

static void
gst_hdbitstream_init (GstHdBitstream * self)
{
  GstAudioDecoder *dec = GST_AUDIO_DECODER (self);

  gst_audio_decoder_set_needs_format (dec, TRUE);
  gst_audio_decoder_set_use_default_pad_acceptcaps (dec, TRUE);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_AUDIO_DECODER_SINK_PAD (dec));
  lgroute_init (&self->route, GST_ELEMENT (self));
}

/* Rank NONE: only adecswitch makes one, by name, when the route is wanted.
 * The category is made here, not in class_init: lgroute.c logs under it from
 * adecswitch before any hdbitstream exists. */
G_GNUC_INTERNAL gboolean
gst_hdbitstream_register (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (hdbitstream_debug, "hdbitstream", 0,
      "lossless bitstream forwarder and LG's eARC route");
  return gst_element_register (plugin, "hdbitstream", GST_RANK_NONE,
      GST_TYPE_HDBITSTREAM);
}
