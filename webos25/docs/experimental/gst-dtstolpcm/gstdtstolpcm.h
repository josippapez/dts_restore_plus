/* gstdtstolpcm.h - DTS -> Blu-ray/HDMV LPCM decoder bin
 *
 * Part of the dts_restore "gst-dtstolpcm" plugin.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __GST_DTS_TO_LPCM_H__
#define __GST_DTS_TO_LPCM_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_DTS_TO_LPCM            (gst_dts_to_lpcm_get_type())
#define GST_DTS_TO_LPCM(obj)           (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_DTS_TO_LPCM,GstDtsToLpcm))
#define GST_DTS_TO_LPCM_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_DTS_TO_LPCM,GstDtsToLpcmClass))
#define GST_IS_DTS_TO_LPCM(obj)        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_DTS_TO_LPCM))
#define GST_IS_DTS_TO_LPCM_CLASS(klass)(G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_DTS_TO_LPCM))

typedef struct _GstDtsToLpcm      GstDtsToLpcm;
typedef struct _GstDtsToLpcmClass GstDtsToLpcmClass;

struct _GstDtsToLpcm
{
  GstBin bin;

  /* internal chain: dcaparse ! avdec_dca ! audioconvert ! audioresample
   *                 ! capsfilter(rate=48000) ! bdlpcmenc */
  GstElement *parse;
  GstElement *dec;
  GstElement *conv;
  GstElement *resample;
  GstElement *capsf;
  GstElement *enc;

  GstPad *sinkpad;   /* ghost -> parse sink */
  GstPad *srcpad;    /* ghost -> enc src    */
};

struct _GstDtsToLpcmClass
{
  GstBinClass parent_class;
};

GType gst_dts_to_lpcm_get_type (void);

G_END_DECLS

#endif /* __GST_DTS_TO_LPCM_H__ */
