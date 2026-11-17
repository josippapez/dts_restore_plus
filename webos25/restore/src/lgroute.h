/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LGROUTE_H
#define LGROUTE_H
#include <gst/gst.h>
#include "lgroute-control.h"

typedef struct {
  GstElement *element;
  GstElement *sink;
  GstPad *sinkpad;
  gulong events, buffers;
  gboolean first, feeding, configured, failed, unmixed, restart_pending, waiting, draining, own;
  gboolean native_es;
  gboolean in_mixer;   /* the kernel feeds eARC from LG's mixer, whatever the mixer word says */
  gboolean rerouted;   /* LG was asked once to route this stream again */
  guint restarts;
  gint64 next_check, mixed_since, configured_at;
  gint64 restart_after;   /* a track-switch restart waits until LG has settled */
  gint64 window_start, last_push;   /* carrier pace check */
  guint64 window_media;
  guint slow_windows;
  const gchar *reason;
  gchar unavailable[LGROUTE_WHY + 32];   /* reason, when eARC became unavailable */
  gchar resource[16], stream_id[128], converter[9];
  gint lease;      /* the renderer index + 1 written to /dev/earc_route */
  int lease_fd;    /* the open /dev/earc_route holding it, or -1 */
} LgRoute;

void lgroute_init (LgRoute *route, GstElement *element);
gboolean lgroute_arm (LgRoute *route);
void lgroute_disarm (LgRoute *route);
GstCaps *lgroute_carrier_caps (void);
/* One line in the tester-visible route log. */
void lgroute_log (const gchar *format, ...) G_GNUC_PRINTF (1, 2);
#endif
