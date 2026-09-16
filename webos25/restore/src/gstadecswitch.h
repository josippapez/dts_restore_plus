/* gstadecswitch.h — audio decoder-switching bin for webOS 25 (LG C5)
 *
 * A GstBin that decodebin3 plugs ONCE for every Dolby/TrueHD/DTS audio stream
 * and then keeps across audio-track switches, hosting LG's `decproxy` (so
 * AC-3/E-AC-3 stay on the hardware DSP) or one of our software decoders
 * (`avdec_truehd`, `avdec_mlp`, `dtsdec`) depending on the current caps.
 *
 * See the file header of gstadecswitch.c for the failure it exists to fix.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __GST_ADECSWITCH_H__
#define __GST_ADECSWITCH_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_ADECSWITCH (gst_adecswitch_get_type ())
G_DECLARE_FINAL_TYPE (GstAdecSwitch, gst_adecswitch, GST, ADECSWITCH, GstBin)

struct _GstAdecSwitch
{
  GstBin bin;

  GstPad *sinkpad;              /* ghost -> current inner decoder sink */
  GstPad *srcpad;               /* ghost -> current inner decoder src  */

  /* Current inner decoder. Owned by the bin; only ever touched from the
   * streaming thread (the sink ghost pad's CAPS probe) plus the async
   * teardown, which is handed its own ref. */
  GstElement *decoder;
  const gchar *decoder_factory; /* static string, or NULL when none yet */

  /* The decproxy we switched away from, kept alive and unlinked instead of
   * being taken to NULL. Taking one to NULL takes over a second on the C5 and
   * kills the audio DSP if a flush lands inside that window, so it only
   * happens when the bin itself leaves PAUSED. Reused when Dolby comes back. */
  GstElement *parked_hw;

  /* Hardware (decproxy) teardown bookkeeping, read by the seek-deferral path
   * on the event thread and written by the async teardown thread. */
  GMutex lock;
  GCond cond;
  guint hw_teardown_pending;    /* decproxy teardowns not yet at NULL */
  gint64 hw_teardown_done_at;   /* g_get_monotonic_time() of the last one */

  /* The most recent `acquired-resource` event seen travelling upstream through
   * the src ghost pad, kept so it can be replayed into an inner decproxy
   * created after it passed. uMS sends it once per pipeline, so a decproxy
   * plugged later never sees it and stays a puppet that outputs nothing.
   * Written and read on the event/streaming threads under `lock`. */
  GstEvent *resource_event;

  /* Properties */
  guint grace_ms;               /* hw-teardown-grace-ms */
  gboolean post_changing_decoder;
};

G_END_DECLS

#endif /* __GST_ADECSWITCH_H__ */
