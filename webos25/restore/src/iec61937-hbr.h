/* iec61937-hbr.h — IEC 61937 high-bit-rate bursts: DTS-HD and Dolby TrueHD
 *
 * Packs a DTS-HD frame (IEC 61937 data type 17, "DTS type IV") or a stream
 * of TrueHD access units (data type 22, in MAT frames) into the bursts an
 * HDMI HBR audio stream carries, byte for byte what FFmpeg 9.0.2's spdif
 * muxer writes:
 *
 *   DTS-HD:  ffmpeg -i in -c:a copy -f spdif -dtshd_rate 768000 out
 *   TrueHD:  ffmpeg -i in -c:a copy -f spdif out
 *
 * CARRIER. 8 channels of S16LE PCM at 192 kHz: 16 bytes per frame, 3,072,000
 * bytes/s, which is what IEC 61937 and FFmpeg call a 768 kHz rate (the same
 * bytes counted as 2-channel IEC 60958 frames of 4 bytes). The burst bytes ARE
 * the carrier's sample bytes, in order: write them into an 8-channel S16LE
 * 192 kHz buffer as is. A burst of N bytes lasts N / 16 carrier frames.
 *
 * BIT-EXACT. The receiver only finds the bursts if every bit arrives as
 * written: no volume, mixing, resampling, dither, channel remapping or format
 * conversion anywhere between here and the HDMI link. One changed bit breaks
 * a preamble or a payload checksum, and the receiver mutes or plays noise.
 *
 * BYTE ORDER (both types). The preamble is four 16-bit little-endian words
 * Pa = 0xF872, Pb = 0x4E1F, Pc = data type | subtype << 8, Pd = length code.
 * The payload is a big-endian byte stream written as little-endian 16-bit
 * words, i.e. each byte pair swapped; an odd last byte becomes the high byte
 * of one more word whose low byte is 0. Zeros fill the burst to its end.
 *
 * DTS-HD (type 17). One burst per complete DTS frame: the 16-bit big-endian
 * core (sync 0x7FFE8001) plus its extension substream, as a parser delivers
 * it. The payload is a 12-byte DTS-HD header (01 00 00 00 00 00 00 00 FE FE,
 * then the frame size as 16-bit big-endian) followed by the frame;
 * Pd = payload bytes rounded up so that (Pd & 0xf) == 8. The burst repeats
 * every 768000 x samples-per-frame / sample-rate IEC 60958 frames:
 *
 *   frame                   period   burst bytes  carrier frames  Pc      max frame
 *   48 kHz, 512 samples      8192      32768         2048        0x0411  32748 bytes
 *   48 kHz, 1024 samples    16384      65536         4096        0x0511  65516 bytes
 *
 * (512 samples at 48 kHz is every Blu-ray DTS-HD track; both last 1/93.75 s
 * or 1/46.875 s, exactly the frame's own duration). Other rates follow the
 * same formula and are refused unless the period is 512, 1024, ... 16384;
 * 44.1 kHz DTS therefore cannot go on this carrier (it needs 176.4 kHz).
 * A frame that does not fit is refused, never truncated: FFmpeg would instead
 * send the core alone for the next 60 s (its -dtshd_fallback_time); that
 * choice is left to the caller.
 *
 * TRUEHD (type 22). Access units are laid into 61424-byte MAT frames by their
 * input timing at a nominal 2560 bytes per 1/1200 s (24 AUs of 40 samples per
 * MAT frame at 48 kHz), with the MAT start, middle and end codes at offsets 0
 * (20 bytes), 30708 (12 bytes) and 61408 (16 bytes). Each complete MAT frame
 * goes out as one burst: Pc = 0x0016, Pd = 61424, 61440 bytes = 3840 carrier
 * frames = 20 ms. Streams at 44.1 kHz multiples produce the same bursts, but
 * at 705.6 kHz, i.e. they need a 176.4 kHz carrier. Packing starts at the
 * first access unit carrying a major sync; earlier ones are refused. FFmpeg
 * drops the MAT frame still being filled when the stream ends; so does this.
 *
 * Plain C99, no allocation, no floating point.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef IEC61937_HBR_H
#define IEC61937_HBR_H

#include <stddef.h>
#include <stdint.h>

#define IEC61937_HBR_CHANNELS       8
#define IEC61937_HBR_RATE           192000
#define IEC61937_HBR_FRAME_BYTES    16          /* 8 x S16LE */
#define IEC61937_HBR_IEC958_RATE    768000      /* FFmpeg's -dtshd_rate */
#define IEC61937_HBR_FRAMES(bytes)  ((bytes) / IEC61937_HBR_FRAME_BYTES)

#define IEC61937_TYPE_DTSHD         0x11
#define IEC61937_TYPE_TRUEHD        0x16

#define IEC61937_DTSHD_MAX_BURST    65536       /* the longest period, 16384 x 4 */
#define IEC61937_MAT_BURST_BYTES    61440
#define IEC61937_MAT_FRAME_BYTES    61424
#define IEC61937_MAT_MAX_AU         8190        /* 12-bit length in 16-bit words */

enum
{
  IEC61937_HBR_EINVAL = -1,     /* not a frame/AU this packer takes */
  IEC61937_HBR_ERATE = -2,      /* DTS: no burst period at 768 kHz for it */
  IEC61937_HBR_E2BIG = -3,      /* DTS: frame does not fit in its burst */
  IEC61937_HBR_ENOSPC = -4,     /* out is smaller than the burst */
  IEC61937_HBR_ENOSYNC = -5     /* TrueHD: no major sync seen yet */
};

/* Packs one DTS frame (core + extension substream) into one burst.
 * With out == NULL only checks the frame. Returns the burst length in bytes
 * (IEC61937_HBR_FRAMES() of it in carrier frames), or a negative
 * IEC61937_HBR_E* code with out untouched. */
int iec61937_dtshd_pack (const uint8_t * frame, size_t size, uint8_t * out,
    size_t out_size);

/* TrueHD MAT packer state. Allocate one per stream (it holds a MAT frame,
 * about 60 KiB), set it up with iec61937_mat_init(), do not touch the fields. */
typedef struct
{
  uint8_t frame[IEC61937_MAT_FRAME_BYTES];
  int filled;                   /* bytes of frame[] written */
  int prev_size;                /* last AU's size, MAT codes included */
  uint16_t prev_time;           /* last AU's input timing */
  int samples_per_frame;        /* from the last major sync; 0 = none yet */
} Iec61937MatPacker;

void iec61937_mat_init (Iec61937MatPacker * mat);

/* Adds one TrueHD access unit. out must hold IEC61937_MAT_BURST_BYTES.
 * Returns 1 when this AU completed a MAT frame and out now holds its
 * 61440-byte burst, 0 when out is unused, or a negative IEC61937_HBR_E* code
 * with the packer unchanged. One AU never completes more than one burst. */
int iec61937_mat_push (Iec61937MatPacker * mat, const uint8_t * au,
    size_t size, uint8_t * out, size_t out_size);

#endif /* IEC61937_HBR_H */
