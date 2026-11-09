/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* When dtsdec decides passthrough again on a track switch. */
#include <assert.h>
#include <stdio.h>
#include "../dtsdec-track.h"

int
main (void)
{
  /* A tester's DTS:X -> DTS -> DTS:X stayed core DTS: core-only decides again. */
  assert (dtsdec_redecide_on_track ("t1", "t2", 1, 0, 0));
  /* A failed HD route decides again on the next track. */
  assert (dtsdec_redecide_on_track ("t1", "t2", 1, 1, 1));
  /* A working HD route is kept; the sink side redoes its setup. */
  assert (!dtsdec_redecide_on_track ("t1", "t2", 1, 1, 0));
  /* The same track, the first track, or nothing decided yet: no change. */
  assert (!dtsdec_redecide_on_track ("t1", "t1", 1, 0, 0));
  assert (!dtsdec_redecide_on_track (NULL, "t1", 1, 0, 0));
  assert (!dtsdec_redecide_on_track ("t1", "t2", 0, 0, 0));
  /* A stream without an id after one with an id is a new track. */
  assert (dtsdec_redecide_on_track ("t1", NULL, 1, 0, 0));
  puts ("test-dtsdec-track: ok");
  return 0;
}
