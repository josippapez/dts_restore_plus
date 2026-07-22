/* gstdtstolpcm.c - DTS -> Blu-ray/HDMV LPCM decoder bin
 *
 * Element name: "dtstolpcm"
 *
 * A manager (GstBin) element that decodes DTS and re-emits it as
 * HDMV/Blu-ray LPCM (audio/x-private-ts-lpcm). Internal chain:
 *
 *   dcaparse ! avdec_dca ! audioconvert ! audioresample
 *            ! capsfilter(rate=48000) ! bdlpcmenc
 *
 * Rationale for the back-door: LG's decproxy/fakedec routes audio to a
 * proprietary MULTICHANNEL decoder only for whitelisted caps. LG deleted
 * every DTS cap from that whitelist, so open decoders (avdec_dca) are
 * pinned to a nerfed stereo sink. But "audio/x-private-ts-lpcm" is still
 * whitelisted and a proprietary LPCM decoder (pcm_audiodec, rank 290) is
 * ready. So we decode DTS -> PCM and re-frame as BD-LPCM; decodebin then
 * autoplugs decproxy -> the proprietary multichannel decoder.
 *
 * A BIN is mandatory: decodebin STOPS autoplugging once it reaches raw
 * audio/x-raw. By keeping our public output non-raw (private-ts-lpcm) the
 * downstream re-enters LG's decproxy autoplug path.
 *
 * License: LGPL v2.1 or later (see header).
 *
 * Targets GStreamer 1.14.x (LG OLED CX). Uses only ghost-pad / bin APIs
 * present since 1.0.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdtstolpcm.h"
#include "gstbdlpcmenc.h"

GST_DEBUG_CATEGORY_STATIC (dtstolpcm_debug);
#define GST_CAT_DEFAULT dtstolpcm_debug

/* Accept plain DTS and DVD/private-stream-1 DTS. NOTE: dcaparse only
 * consumes audio/x-dts; x-private1-dts is listed so decodebin will offer
 * this bin for it, but it requires the upstream LG demux to have already
 * stripped the private-stream-1 substream header (LG's tsdemux does).
 * If you feed raw private1 payload you must add a depayloader ahead of
 * dcaparse. */
static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-dts; audio/x-private1-dts"));

static GstStaticPadTemplate src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-private-ts-lpcm"));

#define gst_dts_to_lpcm_parent_class parent_class
G_DEFINE_TYPE (GstDtsToLpcm, gst_dts_to_lpcm, GST_TYPE_BIN);

static gboolean
gst_dts_to_lpcm_build (GstDtsToLpcm * self)
{
  GstBin *bin = GST_BIN (self);
  GstElement *bdenc;
  GstCaps *rate_caps;
  GstPad *pad;

  self->parse = gst_element_factory_make ("dcaparse", NULL);
  self->dec = gst_element_factory_make ("avdec_dca", NULL);
  self->conv = gst_element_factory_make ("audioconvert", NULL);
  self->resample = gst_element_factory_make ("audioresample", NULL);
  self->capsf = gst_element_factory_make ("capsfilter", NULL);

  /* bdlpcmenc is provided by this same plugin; instantiate by GType so we
   * do not depend on registration ordering. */
  bdenc = g_object_new (GST_TYPE_BDLPCM_ENC, NULL);
  self->enc = bdenc;

  if (!self->parse || !self->dec || !self->conv || !self->resample ||
      !self->capsf || !self->enc) {
    GST_ERROR_OBJECT (self, "missing element(s): dcaparse=%p avdec_dca=%p "
        "audioconvert=%p audioresample=%p capsfilter=%p bdlpcmenc=%p",
        self->parse, self->dec, self->conv, self->resample,
        self->capsf, self->enc);
    /* free anything we did create */
    if (self->parse) gst_object_unref (self->parse);
    if (self->dec) gst_object_unref (self->dec);
    if (self->conv) gst_object_unref (self->conv);
    if (self->resample) gst_object_unref (self->resample);
    if (self->capsf) gst_object_unref (self->capsf);
    if (self->enc) gst_object_unref (self->enc);
    self->parse = self->dec = self->conv = NULL;
    self->resample = self->capsf = self->enc = NULL;
    return FALSE;
  }

  /* Force 48 kHz: BD-LPCM forbids 44.1 kHz. audioresample does the work;
   * the capsfilter pins the negotiated rate. Format/channels flow through
   * (audioconvert will produce S16/S24 as needed by bdlpcmenc's caps). */
  rate_caps = gst_caps_new_simple ("audio/x-raw",
      "rate", G_TYPE_INT, 48000, NULL);
  g_object_set (self->capsf, "caps", rate_caps, NULL);
  gst_caps_unref (rate_caps);

  gst_bin_add_many (bin, self->parse, self->dec, self->conv,
      self->resample, self->capsf, self->enc, NULL);

  if (!gst_element_link_many (self->parse, self->dec, self->conv,
          self->resample, self->capsf, self->enc, NULL)) {
    GST_ERROR_OBJECT (self, "failed to link internal chain");
    return FALSE;
  }

  {
    GstPadTemplate *tmpl;

    /* sink ghost pad -> dcaparse sink */
    pad = gst_element_get_static_pad (self->parse, "sink");
    tmpl = gst_static_pad_template_get (&sink_template);
    self->sinkpad = gst_ghost_pad_new_from_template ("sink", pad, tmpl);
    gst_object_unref (tmpl);
    gst_object_unref (pad);
    gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

    /* src ghost pad -> bdlpcmenc src */
    pad = gst_element_get_static_pad (self->enc, "src");
    tmpl = gst_static_pad_template_get (&src_template);
    self->srcpad = gst_ghost_pad_new_from_template ("src", pad, tmpl);
    gst_object_unref (tmpl);
    gst_object_unref (pad);
    gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  }

  return TRUE;
}

static GstStateChangeReturn
gst_dts_to_lpcm_change_state (GstElement * element, GstStateChange transition)
{
  GstDtsToLpcm *self = GST_DTS_TO_LPCM (element);

  /* The chain is built in instance_init so the ALWAYS ghost pads exist the
   * moment decodebin instantiates us. If that build failed (e.g. avdec_dca
   * not installed) refuse to leave NULL with a clear error. */
  if (transition == GST_STATE_CHANGE_NULL_TO_READY && self->enc == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN,
        ("Required decode elements are not installed."),
        ("Need dcaparse, avdec_dca, audioconvert, audioresample."));
    return GST_STATE_CHANGE_FAILURE;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_dts_to_lpcm_class_init (GstDtsToLpcmClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  /* Klass MUST be Codec/Decoder/Audio so decodebin treats this bin as a
   * DTS decoder and autoplugs it by rank. */
  gst_element_class_set_static_metadata (element_class,
      "DTS to Blu-ray LPCM decoder",
      "Codec/Decoder/Audio",
      "Decodes DTS and re-frames it as HDMV/Blu-ray LPCM so LG webOS routes "
      "it through the proprietary multichannel LPCM sink",
      "dts_restore contributors");

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_dts_to_lpcm_change_state);

  GST_DEBUG_CATEGORY_INIT (dtstolpcm_debug, "dtstolpcm", 0,
      "DTS to Blu-ray LPCM decoder bin");
}

static void
gst_dts_to_lpcm_init (GstDtsToLpcm * self)
{
  self->parse = self->dec = self->conv = NULL;
  self->resample = self->capsf = self->enc = NULL;
  self->sinkpad = self->srcpad = NULL;

  /* Build immediately: decodebin fetches our ALWAYS ghost pads right after
   * instantiation, so they must already exist. On failure self->enc stays
   * NULL and change_state() will error out cleanly. */
  if (!gst_dts_to_lpcm_build (self))
    GST_WARNING_OBJECT (self, "failed to build internal DTS decode chain");
}
