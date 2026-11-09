/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LGROUTE_H
#define LGROUTE_H
#include <gst/gst.h>
#include "lgroute-control.h"

typedef struct LgSession LgSession;

typedef struct {
  GstElement *element;
  GstElement *sink;
  GstPad *sinkpad;
  gulong events, buffers;
  gboolean first, feeding, configured, failed, unmixed, restart_pending;
  guint restarts;
  gint64 next_check, mixed_since, configured_at;
  gint64 window_start, last_push;   /* carrier pace check */
  guint64 window_media;
  guint slow_windows;
  const gchar *reason;
  gchar resource[16], stream_id[128], converter[9];
  LgSession *session;
} LgRoute;

void lgroute_init (LgRoute *route, GstElement *element);
gboolean lgroute_arm (LgRoute *route);
void lgroute_disarm (LgRoute *route);
GstCaps *lgroute_carrier_caps (void);
/* One line in the tester-visible route log. */
void lgroute_log (const gchar *format, ...) G_GNUC_PRINTF (1, 2);
#endif
