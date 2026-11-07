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
int lgroute_control_receiver (void);
int lgroute_sound_output (char mode[64]);
#endif
