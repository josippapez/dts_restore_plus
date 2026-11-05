/* iec61937-hbr.c — IEC 61937 high-bit-rate bursts: DTS-HD and Dolby TrueHD
 *
 * See iec61937-hbr.h for the carrier, the burst sizes and the byte order.
 *
 * A port of the DTS type IV and TrueHD/MAT paths of FFmpeg's IEC 61937
 * muxer, libavformat/spdifenc.c at tag n9.0.2 (the file is identical on
 * release/9.0), checked byte for byte against that version by
 * test/test-iec61937-hbr.c. FFmpeg master later reworked the TrueHD padding
 * (commit 2db563fac2); this follows 9.0.2, not master.
 *
 *   spdifenc.c: Copyright (c) 2009 Bartlomiej Wolowiec
 *               Copyright (c) 2010, 2020 Anssi Hannula
 *               Copyright (c) 2010 Carl Eugen Hoyos
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "iec61937-hbr.h"

#include <string.h>

#define SYNCWORD1           0xF872
#define SYNCWORD2           0x4E1F
#define BURST_HEADER_SIZE   8
#define DTS_SYNC_CORE_BE    0x7FFE8001u

static void
put_le16 (uint8_t * p, unsigned v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}

static void
put_preamble (uint8_t * out, unsigned type, unsigned length_code)
{
  put_le16 (out, SYNCWORD1);
  put_le16 (out + 2, SYNCWORD2);
  put_le16 (out + 4, type);
  put_le16 (out + 6, length_code);
}

/* n big-endian payload bytes as little-endian words. An odd last byte goes in
 * the high byte of one more word, so n + 1 bytes of out are written. */
static void
put_swapped (uint8_t * out, const uint8_t * in, size_t n)
{
  size_t i;

  for (i = 0; i + 1 < n; i += 2) {
    out[i] = in[i + 1];
    out[i + 1] = in[i];
  }
  if (n & 1) {
    out[n - 1] = 0;
    out[n] = in[n - 1];
  }
}

/* ff_dca_sample_rates[] */
static const uint32_t dts_sample_rates[16] = {
  0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0,
  12000, 24000, 48000, 96000, 192000
};

int
iec61937_dtshd_pack (const uint8_t * frame, size_t size, uint8_t * out,
    size_t out_size)
{
  /* spdif_header_dts4()'s dtshd_start_code[], then the frame size */
  uint8_t hdr[12] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xfe, 0xfe
  };
  uint32_t rate;
  uint64_t period;
  unsigned samples, subtype;
  size_t burst, payload;

  if (size < 9 || ((uint32_t) frame[0] << 24 | (uint32_t) frame[1] << 16
          | (uint32_t) frame[2] << 8 | frame[3]) != DTS_SYNC_CORE_BE)
    return IEC61937_HBR_EINVAL;
  samples = ((((unsigned) frame[4] << 8 | frame[5]) >> 2 & 0x7f) + 1) << 5;
  rate = dts_sample_rates[(frame[8] >> 2) & 0x0f];
  if (rate == 0)
    return IEC61937_HBR_EINVAL;

  /* FFmpeg multiplies in int, which overflows above 2796 samples per frame.
   * Both then refuse the frame, except 4096 samples at rate code 15
   * (192 kHz): period 16384 here, a wrapped negative one there. */
  period = (uint64_t) IEC61937_HBR_IEC958_RATE * samples / rate;
  for (subtype = 0; subtype <= 5 && period != 512u << subtype; subtype++);
  if (subtype > 5)
    return IEC61937_HBR_ERATE;
  burst = (size_t) period * 4;

  if (size > burst - BURST_HEADER_SIZE - sizeof hdr)
    return IEC61937_HBR_E2BIG;
  if (out == NULL)
    return (int) burst;
  if (out_size < burst)
    return IEC61937_HBR_ENOSPC;

  hdr[10] = (size >> 8) & 0xff;
  hdr[11] = size & 0xff;
  payload = sizeof hdr + size;
  memset (out, 0, burst);
  /* "(length_code & 0xf) == 0x8 ... reportedly needed with some receivers" */
  put_preamble (out, IEC61937_TYPE_DTSHD | subtype << 8,
      (payload + 0x8 + 0xf) / 0x10 * 0x10 - 0x8);
  put_swapped (out + BURST_HEADER_SIZE, hdr, sizeof hdr);
  put_swapped (out + BURST_HEADER_SIZE + sizeof hdr, frame, size);
  return (int) burst;
}

static const uint8_t mat_start_code[20] = {
  0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00, 0x56, 0xA5,
  0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0
};

static const uint8_t mat_middle_code[12] = {
  0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0
};

static const uint8_t mat_end_code[16] = {
  0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x97, 0x11
};

static const struct
{
  int pos;
  int len;
  const uint8_t *code;
} mat_codes[3] = {
  {0, sizeof mat_start_code, mat_start_code},
  {30708, sizeof mat_middle_code, mat_middle_code},
  {IEC61937_MAT_FRAME_BYTES - sizeof mat_end_code, sizeof mat_end_code,
      mat_end_code},
};

void
iec61937_mat_init (Iec61937MatPacker * mat)
{
  memset (mat, 0, sizeof *mat);
}

/* spdif_header_truehd(), with the finished MAT frame written out at once
 * instead of through FFmpeg's second buffer. */
int
iec61937_mat_push (Iec61937MatPacker * mat, const uint8_t * au, size_t size,
    uint8_t * out, size_t out_size)
{
  const uint8_t *data = au;
  int total = (int) size, data_left = (int) size, padding = 0;
  int next, have_burst = 0;
  uint16_t timing;

  if (out == NULL || out_size < IEC61937_MAT_BURST_BYTES)
    return IEC61937_HBR_ENOSPC;
  /* The upper bound, the most an AU's length field can say, is also what
   * keeps one AU to at most one burst: it plus the largest padding allowed
   * below is less than a MAT frame. */
  if (size < 10 || size > IEC61937_MAT_MAX_AU)
    return IEC61937_HBR_EINVAL;

  if (au[4] == 0xf8 && au[5] == 0x72 && au[6] == 0x6f) {
    /* major sync: the sample rate code gives the AU length in samples */
    unsigned ratebits;

    if (au[7] == 0xba)
      ratebits = au[8] >> 4;
    else if (au[7] == 0xbb)
      ratebits = au[9] >> 4;
    else
      return IEC61937_HBR_EINVAL;
    mat->samples_per_frame = 40 << (ratebits & 3);
  }
  if (mat->samples_per_frame == 0)
    return IEC61937_HBR_ENOSYNC;

  timing = (uint16_t) (au[2] << 8 | au[3]);
  if (mat->prev_size) {
    /* 2560 bytes of the 768 kHz stream per 1/1200 s (1/1102.5 s at 705.6) */
    uint16_t delta_samples = timing - mat->prev_time;
    int delta_bytes = delta_samples * 2560 / mat->samples_per_frame;

    padding = delta_bytes - mat->prev_size;
    /* FFmpeg's "Unusual frame timing": place this AU right after the last */
    if (padding < 0 || padding >= IEC61937_MAT_FRAME_BYTES / 2)
      padding = 0;
  }

  for (next = 0; next < 2 && mat->filled > mat_codes[next].pos; next++);

  while (padding || data_left || mat_codes[next].pos == mat->filled) {
    if (mat_codes[next].pos == mat->filled) {
      int code_left = mat_codes[next].len;

      memcpy (mat->frame + mat->filled, mat_codes[next].code, code_left);
      mat->filled += code_left;
      if (++next == 3) {
        next = 0;
        put_preamble (out, IEC61937_TYPE_TRUEHD, IEC61937_MAT_FRAME_BYTES);
        put_swapped (out + BURST_HEADER_SIZE, mat->frame,
            IEC61937_MAT_FRAME_BYTES);
        memset (out + BURST_HEADER_SIZE + IEC61937_MAT_FRAME_BYTES, 0,
            IEC61937_MAT_BURST_BYTES - BURST_HEADER_SIZE
            - IEC61937_MAT_FRAME_BYTES);
        have_burst = 1;
        mat->filled = 0;
        /* the gap between MAT frames counts too */
        code_left += IEC61937_MAT_BURST_BYTES - IEC61937_MAT_FRAME_BYTES;
      }
      if (padding) {
        /* a MAT code stands in for padding where padding is due */
        int n = padding < code_left ? padding : code_left;

        padding -= n;
        code_left -= n;
      }
      total += code_left;
    }

    if (padding) {
      int n = mat_codes[next].pos - mat->filled;

      if (n > padding)
        n = padding;
      memset (mat->frame + mat->filled, 0, n);
      mat->filled += n;
      padding -= n;
      if (padding)
        continue;               /* a MAT code is due first */
    }

    if (data_left) {
      int n = mat_codes[next].pos - mat->filled;

      if (n > data_left)
        n = data_left;
      memcpy (mat->frame + mat->filled, data, n);
      mat->filled += n;
      data += n;
      data_left -= n;
    }
  }

  mat->prev_size = total;
  mat->prev_time = timing;
  return have_burst;
}
