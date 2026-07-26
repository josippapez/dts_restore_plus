/* GStreamer DTS decoder plugin based on libdtsdec
 * Copyright (C) 2004 Ronald Bultje <rbultje@ronald.bitfreak.net>
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

/* Vendored from gst-plugins-bad 1.22.0 (ext/dts/gstdtsdec.h). Only changes
 * from upstream are the make-up-gain and DRC/center-boost fields below
 * (webOS 25 patch).
 * See gstdtsdec.c file header for the full list of functional changes. */

#ifndef __GST_DTSDEC_H__
#define __GST_DTSDEC_H__

#include <gst/gst.h>
#include <gst/audio/gstaudiodecoder.h>

G_BEGIN_DECLS

#define GST_TYPE_DTSDEC \
  (gst_dtsdec_get_type())
#define GST_DTSDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_DTSDEC,GstDtsDec))
#define GST_DTSDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_DTSDEC,GstDtsDecClass))
#define GST_IS_DTSDEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_DTSDEC))
#define GST_IS_DTSDEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_DTSDEC))

typedef struct _GstDtsDec GstDtsDec;
typedef struct _GstDtsDecClass GstDtsDecClass;

struct _GstDtsDec {
  GstAudioDecoder	 element;

  GstPadChainFunction base_chain;

  gboolean       dvdmode;
  gboolean       flag_update;
  gboolean       prev_flags;

  /* stream properties */
  gint 	         bit_rate;
  gint 	         sample_rate;
  gint 	         stream_channels;
  gint 	         request_channels;
  gint 	         using_channels;

  gint           channel_reorder_map[6];

  /* decoding properties */
  sample_t 	 level;
  sample_t 	 bias;
  gboolean 	 dynamic_range_compression;

  /* webOS 25 patch: user-tunable make-up gain. makeup_gain_db is the
   * clamped [-20,+20] dB value (also the get-property value);
   * makeup_gain_linear is the cached pow(10, dB/20) multiplier applied in
   * the float->S32 output loop (1.0f exactly when makeup_gain_db == 0.0). */
  gfloat 	 makeup_gain_db;
  gfloat 	 makeup_gain_linear;

  /* webOS 25 patch: DRC compressor + center-channel (dialogue) boost.
   * User settings — see the "DRC CORE" block in gstdtsdec.c for the contract.
   * drc_mode is a DTS_DRC_MODE_* value (0=off, 1=line, 2=rf); it is a gint
   * here so this header stays free of the .c-private enum. */
  gint  	 drc_mode;
  gfloat 	 drc_boost_pct;	  /* 0..100, scales positive (boost) gains */
  gfloat 	 drc_cut_pct;	  /* 0..100, scales negative (cut) gains    */
  gfloat 	 center_boost_db; /* -10..+10 dB, front-centre channel only  */
  gfloat 	 center_boost_linear;	/* cached; exactly 1.0f when 0.0 dB */

  /* webOS 25 patch: DRC runtime state (per stream, reset in _start()). */
  gfloat 	 drc_smoothed_db;   /* one-pole smoothed gain, dB domain     */
  gfloat 	 drc_prev_linear;   /* previous block's linear DRC factor    */
  gfloat 	 drc_attack_coef;   /* per-block one-pole coefficients,      */
  gfloat 	 drc_release_coef;  /*   derived from mode + sample rate     */
  gint  	 drc_coef_rate;	    /* sample rate the coefs were built for  */
  gint  	 drc_coef_mode;	    /* drc_mode the coefs were built for     */

  sample_t 	*samples;
#ifndef DTS_OLD
  dca_state_t   *state;
#else
  dts_state_t 	*state;
#endif
};

struct _GstDtsDecClass {
  GstAudioDecoderClass parent_class;

  guint32 dts_cpuflags;
};

GType gst_dtsdec_get_type(void);

GST_ELEMENT_REGISTER_DECLARE (dtsdec);

G_END_DECLS

#endif /* __GST_DTSDEC_H__ */
