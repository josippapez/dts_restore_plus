/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef EDIDAUDIO_H
#define EDIDAUDIO_H
#include <stdint.h>

/* eim runs as root, so the verdict goes under root-only /run, not /tmp. */
#define EDIDAUDIO_DIR "/run/dts25"
#define EDIDAUDIO_STATE EDIDAUDIO_DIR "/edidaudio"

struct edidaudio_range { uintptr_t lo, hi; };

/* edidType numbers in libhdmi-conn-protocol's property table. */
struct edidaudio_map { int truehd_dts, truehd, ac3_dts, ac3; };

/* Finds {"audio_edid_type", value, number} entries in the readable ranges.
 * Returns 1 and fills *m only when all four values are there and the "+dts"
 * forms differ from the plain ones. */
int edidaudio_scan (const struct edidaudio_range *r, int n, struct edidaudio_map *m);
#endif
