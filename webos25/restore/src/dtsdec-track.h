/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef DTSDEC_TRACK_H
#define DTSDEC_TRACK_H
#include <string.h>

/* On a new upstream track (stream id changed), dtsdec decides passthrough
 * again unless it is on a working HD route: adecswitch keeps the same decoder
 * across DTS tracks, so a core-only or failed choice would otherwise stick. */
static inline int
dtsdec_redecide_on_track (const char *old_id, const char *new_id, int decided,
    int hd, int failed)
{
  if (!old_id || (new_id && !strcmp (old_id, new_id)) || !decided)
    return 0;
  return !hd || failed;
}
#endif
