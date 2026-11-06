/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LGROUTE_CONTROL_H
#define LGROUTE_CONTROL_H

#ifndef LGROUTE_RUN
#define LGROUTE_RUN "/run/dts25"
#endif
#define LGROUTE_LOCK LGROUTE_RUN "/hd.lock"
#define LGROUTE_STATE LGROUTE_RUN "/lgroute.state"
#ifndef LGROUTE_HD
#define LGROUTE_HD "/var/lib/webosbrew/dts25/passthrough-hd"
#endif
#ifndef LGROUTE_PT
#define LGROUTE_PT "/var/lib/webosbrew/dts25/passthrough"
#endif
#define LGROUTE_OUTPUT LGROUTE_RUN "/sound-output"
#define LGROUTE_SINK_OK LGROUTE_RUN "/sink-ok"

typedef struct {
  int fd;
  int lock_fd;
} LgRouteControl;

int lgroute_control_check (void);
int lgroute_control_open (LgRouteControl *control);
int lgroute_control_apply (LgRouteControl *control);
int lgroute_control_release (LgRouteControl *control);
int lgroute_control_recover (void);
int lgroute_control_receiver (void);
#endif
