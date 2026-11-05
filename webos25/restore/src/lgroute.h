/* lgroute.h — LG's own eARC bitstream route, driven from the producer (webOS 25)
 *
 * Approach A of the lossless route: no kernel module. LG's lgaudiosink is made
 * to build its PCM bypass head (pcm-multi-ch TRUE while the carrier CAPS pass
 * through it) so that LG's sound-output code connects renderer -> mixer ->
 * eARC itself, and the flag is cleared again before the first buffer so that
 * the first event on the bypass is ES/HBR and the eARC output module puts
 * itself in non-audio HBR mode. The reasoning and the decomp lines are in
 * lgroute-policy.h.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __LGROUTE_H__
#define __LGROUTE_H__

#include <gst/gst.h>
#include "lgroute-policy.h"

G_BEGIN_DECLS

#define LGROUTE_MARKER       "/var/lib/webosbrew/dts25/lgroute-test"
#define LGROUTE_SOUND_OUTPUT "/run/dts25/sound-output"
#define LGROUTE_LOG          "/tmp/dts25-lgroute.log"

typedef struct
{
  GstElement *element;          /* the producer, for logging only */
  GstElement *sink;             /* LG's audiosink, held while armed */
  GstPad *sinkpad;              /* its sink pad, where the probes sit */
  gulong event_probe;
  gulong buffer_probe;
  LgRouteFlip flip;
} LgRoute;

/* The opt-in marker exists and the watcher reports an ARC/eARC receiver with
 * Pass Through or Auto (or the marker says "force"). */
G_GNUC_INTERNAL gboolean lgroute_wanted (void);

/* GST_INFO on @element plus a line in LGROUTE_LOG. */
G_GNUC_INTERNAL void lgroute_log (GstElement * element, const gchar * fmt, ...)
    G_GNUC_PRINTF (2, 3);

/* The 8-channel 16-bit 192 kHz carrier caps with the ES description. */
G_GNUC_INTERNAL GstCaps *lgroute_carrier_caps (void);

G_GNUC_INTERNAL void lgroute_init (LgRoute * route, GstElement * element);

/* Finds the single LG audiosink above @element and puts the flip probes on
 * its sink pad. Call before the carrier caps are pushed. */
G_GNUC_INTERNAL gboolean lgroute_arm (LgRoute * route, const gchar ** why);

/* Removes the probes and clears pcm-multi-ch if we left it set. Safe to call
 * repeatedly. */
G_GNUC_INTERNAL void lgroute_disarm (LgRoute * route);

G_END_DECLS

#endif /* __LGROUTE_H__ */
