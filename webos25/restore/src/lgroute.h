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
  gboolean first, feeding, configured, failed;
  gint64 next_check, mixed_since;
  const gchar *reason;
} LgRoute;

void lgroute_init (LgRoute *route, GstElement *element);
gboolean lgroute_arm (LgRoute *route);
void lgroute_disarm (LgRoute *route);
GstCaps *lgroute_carrier_caps (void);
#endif
