/* gstbdlpcmenc.h - Blu-ray/HDMV LPCM audio encoder element
 *
 * Part of the dts_restore "gst-dtstolpcm" plugin.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * (Same license as the GStreamer project and its plugins.)
 */

#ifndef __GST_BDLPCM_ENC_H__
#define __GST_BDLPCM_ENC_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudioencoder.h>

G_BEGIN_DECLS

#define GST_TYPE_BDLPCM_ENC            (gst_bdlpcm_enc_get_type())
#define GST_BDLPCM_ENC(obj)           (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BDLPCM_ENC,GstBdLpcmEnc))
#define GST_BDLPCM_ENC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BDLPCM_ENC,GstBdLpcmEncClass))
#define GST_IS_BDLPCM_ENC(obj)        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BDLPCM_ENC))
#define GST_IS_BDLPCM_ENC_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BDLPCM_ENC))

typedef struct _GstBdLpcmEnc      GstBdLpcmEnc;
typedef struct _GstBdLpcmEncClass GstBdLpcmEncClass;

struct _GstBdLpcmEnc
{
  GstAudioEncoder parent;

  /* negotiated input */
  GstAudioInfo info;
  gboolean     configured;

  /* derived BD-LPCM stream parameters */
  gint     in_channels;     /* channels coming in from audioconvert       */
  gint     bd_channels;     /* channels written to the stream (2/6/8)      */
  gint     ca_code;         /* channel-assignment nibble (3/9/11)          */
  gint     sr_code;         /* sample-rate nibble (1/4/5)                   */
  gint     bps_code;        /* bits-per-sample nibble (1=16-bit, 3=24-bit) */
  gint     bytes_per_sample;/* 2 or 3                                       */
  gint     frame_samples;   /* per-channel samples per BD frame (240/360)  */
  gboolean swap;            /* TRUE if input endianness != big-endian      */

  /* reorder[bd_out_channel] = input channel index, or -1 to emit silence.
   * BD-LPCM carries LFE LAST, which differs from GStreamer's SMPTE order
   * where LFE is 4th; this table performs that remap. */
  gint     reorder[8];
};

struct _GstBdLpcmEncClass
{
  GstAudioEncoderClass parent_class;
};

GType gst_bdlpcm_enc_get_type (void);

G_END_DECLS

#endif /* __GST_BDLPCM_ENC_H__ */
