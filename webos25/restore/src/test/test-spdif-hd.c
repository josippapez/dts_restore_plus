/* Host test: dtsdec's HD passthrough setup of FFmpeg's spdif muxer, fed whole
 * DTS-HD MA frames, must emit IEC 61937 type IV bursts at the 768 kHz HBR rate
 * in big-endian words, carrying each frame byte for byte. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/opt.h>

static uint8_t out[1 << 22];
static int out_len;

static int sink (void *opaque, uint8_t * buf, int size)
{
  (void) opaque;
  memcpy (out + out_len, buf, size);
  out_len += size;
  return size;
}

int main (int argc, char **argv)
{
  static uint8_t in[1 << 21];
  AVFormatContext *s = NULL;
  AVStream *st;
  AVPacket *pkt = av_packet_alloc ();
  FILE *f;
  long n, off = 0;
  int frames = 0, len, period, ret;

  if (argc != 2 || !(f = fopen (argv[1], "rb"))) {
    fprintf (stderr, "usage: %s <raw dts-hd file>\n", argv[0]);
    return 2;
  }
  n = (long) fread (in, 1, sizeof (in), f);
  fclose (f);

  if (avformat_alloc_output_context2 (&s, NULL, "spdif", NULL) < 0)
    return 1;
  /* FFmpeg 7 made the buffer const; dtsdec builds against n4.4 */
  s->pb = avio_alloc_context (av_malloc (4096), 4096, 1, NULL, NULL,
      (void *) sink, NULL);
  st = avformat_new_stream (s, NULL);
  st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  st->codecpar->codec_id = AV_CODEC_ID_DTS;
  st->codecpar->sample_rate = 48000;
  av_opt_set_int (s->priv_data, "dtshd_rate", 768000, 0);
  av_opt_set_int (s->priv_data, "dtshd_fallback_time", 0, 0);
  av_opt_set (s->priv_data, "spdif_flags", "be", 0);
  if (avformat_write_header (s, NULL) < 0)
    return 1;

  /* raw .dts: core sync 7FFE8001 at each frame start, EXSS follows */
  while (off + 16 < n && frames < 32) {
    long next = off + 1;
    while (next + 4 < n && !(in[next] == 0x7f && in[next + 1] == 0xfe
            && in[next + 2] == 0x80 && in[next + 3] == 0x01))
      next++;
    if (next + 4 >= n)
      next = n;
    pkt->data = in + off;
    pkt->size = (int) (next - off);
    len = out_len;
    ret = av_write_frame (s, pkt);
    avio_flush (s->pb);
    if (ret < 0) {
      printf ("FAIL frame %d: av_write_frame %d\n", frames, ret);
      return 1;
    }
    period = out_len - len;
    /* Pa Pb big-endian, Pc = 0x11 | subtype<<8, then the DTS-HD start code */
    if (out[len] != 0xF8 || out[len + 1] != 0x72 || out[len + 2] != 0x4E
        || out[len + 3] != 0x1F || (out[len + 5] & 0x7f) != 0x11
        || out[len + 8] != 0x01 || out[len + 16] != 0xfe
        || (out[len + 18] << 8 | out[len + 19]) != pkt->size
        || memcmp (out + len + 20, in + off, (size_t) pkt->size) != 0) {
      printf ("FAIL frame %d: burst header or payload mismatch\n", frames);
      return 1;
    }
    if (frames == 0)
      printf ("burst period %d bytes, subtype %d, frame %d bytes\n", period,
          out[len + 4] & 0x1f, pkt->size);
    off = next;
    frames++;
  }
  /* dtsdec mutes a frame whose burst length field is not its own size: an
   * oversized frame must come out core-only, so that check must catch it */
  {
    static uint8_t big[40000];
    int core = ((in[5] << 16 | in[6] << 8 | in[7]) >> 4 & 0x3fff) + 1;

    memcpy (big, in, (size_t) core);
    memset (big + core, 0x55, sizeof (big) - (size_t) core);
    pkt->data = big;
    pkt->size = sizeof (big);
    len = out_len;
    if (av_write_frame (s, pkt) < 0)
      return 1;
    avio_flush (s->pb);
    if ((out[len + 18] << 8 | out[len + 19]) == pkt->size) {
      printf ("FAIL oversized frame was not detectable as stripped\n");
      return 1;
    }
    printf ("ok - oversized frame stripped to %d bytes, detectable by length\n",
        out[len + 18] << 8 | out[len + 19]);
  }
  av_write_trailer (s);
  printf ("ok - %d DTS-HD frames packed whole as IEC 61937 type IV (BE, 768 kHz)\n",
      frames);
  return frames > 0 ? 0 : 1;
}
