/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LGROUTE_CONTROL_H
#define LGROUTE_CONTROL_H

#ifndef LGROUTE_RUN
#define LGROUTE_RUN "/run/dts25"
#endif
#ifndef LGROUTE_HD
#define LGROUTE_HD "/var/lib/webosbrew/dts25/passthrough-hd"
#endif
#ifndef LGROUTE_PT
#define LGROUTE_PT "/var/lib/webosbrew/dts25/passthrough"
#endif
#define LGROUTE_OUTPUT LGROUTE_RUN "/sound-output"
#define LGROUTE_SINK_OK LGROUTE_RUN "/sink-ok"

int lgroute_control_unmixed (void);
int lgroute_control_unmix (void);
/* The same; on -1 why names the control and what was read from it. */
#define LGROUTE_WHY 128
int lgroute_control_unmixed_why (char why[LGROUTE_WHY]);
int lgroute_control_receiver (void);
int lgroute_sound_output (char mode[64]);
int lgroute_core_output (char mode[64]);
#endif
