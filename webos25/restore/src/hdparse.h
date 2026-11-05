/* hdparse.h — frame delimiting for the lossless forwarder (hdbitstream)
 *
 * Plain C, no GStreamer, so test/test-hdparse.c can run it on the host.
 *
 * DTS: a frame is a 16-bit big-endian core (sync 0x7FFE8001) plus the DTS-HD
 * extension substream (EXSS, sync 0x64582025) that follows it, as the dca
 * parser delivers it: the same sizing as gstdtsdec.c's dts_frame_size(),
 * without the little-endian and 14-bit forms (the type 17 packer takes only
 * the big-endian 16-bit core, so those decode instead).
 *
 * TrueHD/MLP: an access unit starts with a 16-bit word whose low 12 bits are
 * its length in 16-bit words; a major sync (F8 72 6F BA for TrueHD, BB for
 * MLP) sits at byte 4 of some of them. As FFmpeg's mlp parser does, a major
 * sync is required to start, and lengths are trusted after that.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __HDPARSE_H__
#define __HDPARSE_H__

#include <stddef.h>
#include <stdint.h>

#define HDPARSE_DTS_SYNC_CORE 0x7FFE8001u
#define HDPARSE_DTS_SYNC_EXSS 0x64582025u
#define HDPARSE_DTS_MIN_CORE 96

/* The packer's bounds (iec61937_mat_push): a shorter AU would be refused
 * there and end the stream, so it is not a frame here either. */
#define HDPARSE_MLP_MIN_AU 10
#define HDPARSE_MLP_MAX_AU 8190

static inline uint32_t
hdparse_be32 (const uint8_t * d)
{
  return (uint32_t) d[0] << 24 | (uint32_t) d[1] << 16 | (uint32_t) d[2] << 8
      | d[3];
}

/* Core FSIZE+1; -1 more data needed, 0 no core header here. */
static inline int
hdparse_dts_core (const uint8_t * d, int avail)
{
  unsigned w2, w3;
  int fsize;

  if (avail < 8)
    return -1;
  if (hdparse_be32 (d) != HDPARSE_DTS_SYNC_CORE)
    return 0;
  w2 = (unsigned) d[4] << 8 | d[5];
  w3 = (unsigned) d[6] << 8 | d[7];
  if ((w2 & 0xFC00) != 0xFC00)  /* FTYPE=1, SHORT=31 */
    return 0;
  fsize = (int) (((w2 & 0x3) << 12) | (w3 >> 4)) + 1;
  return fsize < HDPARSE_DTS_MIN_CORE ? 0 : fsize;
}

/* EXSS nuExtSSFsize+1; -1 more data needed, 0 no EXSS header here. */
static inline int
hdparse_dts_exss (const uint8_t * d, int avail)
{
  uint64_t v = 0;
  int i;

  if (avail < 4)
    return -1;
  if (hdparse_be32 (d) != HDPARSE_DTS_SYNC_EXSS)
    return 0;
  if (avail < 10)
    return -1;
  for (i = 4; i < 10; i++)
    v = (v << 8) | d[i];
  if (v & ((uint64_t) 1 << 37))
    return (int) ((v >> 5) & 0xFFFFF) + 1;
  return (int) ((v >> 13) & 0xFFFF) + 1;
}

/* The DTS frame at d: core plus a following EXSS, or an EXSS alone.
 * -1 more data needed, 0 no frame starts here. */
static inline int
hdparse_dts_frame (const uint8_t * d, int avail)
{
  int core = hdparse_dts_core (d, avail), ext;

  if (core == -1 && avail >= 4 && hdparse_be32 (d) == HDPARSE_DTS_SYNC_EXSS)
    core = 0;
  if (core < 0)
    return -1;
  if (core == 0)
    return hdparse_dts_exss (d, avail);
  if (avail < core + 4)
    return -1;
  ext = hdparse_dts_exss (d + core, avail - core);
  if (ext < 0)
    return -1;
  return core + ext;
}

static inline int
hdparse_mlp_has_sync (const uint8_t * d, int avail)
{
  return avail >= 8 && d[4] == 0xf8 && d[5] == 0x72 && d[6] == 0x6f
      && (d[7] == 0xba || d[7] == 0xbb);
}

/* The access unit at d, from its length word; 0 if the length is not one an
 * access unit can have. With need_sync, 0 unless a major sync is at byte 4. */
static inline int
hdparse_mlp_au (const uint8_t * d, int avail, int need_sync)
{
  int length;

  if (avail < 8)
    return -1;
  if (need_sync && !hdparse_mlp_has_sync (d, avail))
    return 0;
  length = (((unsigned) d[0] << 8 | d[1]) & 0xfff) * 2;
  if (length < HDPARSE_MLP_MIN_AU || length > HDPARSE_MLP_MAX_AU)
    return 0;
  return length;
}

#endif /* __HDPARSE_H__ */
