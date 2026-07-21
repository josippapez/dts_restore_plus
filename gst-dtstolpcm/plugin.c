/* plugin.c - GStreamer plugin registration for gst-dtstolpcm
 *
 * Registers two elements:
 *   - bdlpcmenc  (GST_RANK_NONE)  raw PCM -> HDMV/Blu-ray LPCM encoder
 *   - dtstolpcm  (rank 310)       DTS -> BD-LPCM decoder bin
 *
 * dtstolpcm's rank (310) MUST exceed avdec_dca's rank (290) so decodebin
 * autoplugs this bin ahead of the plain DTS decoder. GST_RANK_PRIMARY is
 * 256; 310 == GST_RANK_PRIMARY + 54.
 *
 * License: LGPL v2.1 or later.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstbdlpcmenc.h"
#include "gstdtstolpcm.h"

/* Must outrank avdec_dca (290) to win decodebin autoplug. */
#define DTSTOLPCM_RANK ((GstRank) 310)

#ifndef VERSION
#define VERSION "1.0.0"
#endif
#ifndef PACKAGE
#define PACKAGE "gst-dtstolpcm"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "DTS to Blu-ray LPCM plugin"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/lgstreamer/dts_restore"
#endif

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ok = TRUE;

  ok &= gst_element_register (plugin, "bdlpcmenc",
      GST_RANK_NONE, GST_TYPE_BDLPCM_ENC);

  ok &= gst_element_register (plugin, "dtstolpcm",
      DTSTOLPCM_RANK, GST_TYPE_DTS_TO_LPCM);

  return ok;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    dtstolpcm,
    "Decode DTS and re-frame as HDMV/Blu-ray LPCM for LG webOS multichannel",
    plugin_init,
    VERSION,
    "LGPL",
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
