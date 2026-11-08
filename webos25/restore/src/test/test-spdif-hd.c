#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/opt.h>

/* av_write_frame() result for a 2012-byte 48 kHz DTS core of the given NBLKS. */
static int write_core (int nblks)
{
  uint8_t core[2012] = { 0x7f, 0xfe, 0x80, 0x01, 0x80 | (nblks >> 6), (nblks & 0x3f) << 2, 0x7d, 0xb0, 13 << 2 };
  uint8_t *burst = NULL;
  AVFormatContext *mux = NULL;
  AVStream *stream;
  AVPacket *packet = av_packet_alloc ();
  int result;

  assert (avformat_alloc_output_context2 (&mux, NULL, "spdif", NULL) >= 0);
  stream = avformat_new_stream (mux, NULL);
  assert (stream && packet);
  stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  stream->codecpar->codec_id = AV_CODEC_ID_DTS;
  stream->codecpar->sample_rate = 48000;
  assert (av_opt_set_int (mux->priv_data, "dtshd_rate", 768000, 0) >= 0);
  assert (av_opt_set_int (mux->priv_data, "dtshd_fallback_time", 0, 0) >= 0);
  assert (avio_open_dyn_buf (&mux->pb) >= 0);
  assert (avformat_write_header (mux, NULL) >= 0);
  packet->data = core;
  packet->size = sizeof (core);
  result = av_write_frame (mux, packet);
  packet->data = NULL;
  packet->size = 0;
  avio_close_dyn_buf (mux->pb, &burst);
  mux->pb = NULL;
  av_free (burst);
  av_packet_free (&packet);
  avformat_free_context (mux);
  return result;
}

int main (int argc, char **argv)
{
  uint8_t frame[40000], *burst = NULL;
  AVFormatContext *mux = NULL;
  AVStream *stream;
  AVPacket *packet = av_packet_alloc ();
  FILE *input = argc == 2 ? fopen (argv[1], "rb") : NULL;
  int core_size, burst_size, packed_size;

  assert (input && packet);
  assert (fread (frame, 1, 8, input) == 8);
  core_size = ((frame[5] << 16 | frame[6] << 8 | frame[7]) >> 4 & 0x3fff) + 1;
  assert (core_size >= 8 && core_size < (int) sizeof (frame));
  assert (fread (frame + 8, 1, core_size - 8, input) == (size_t) core_size - 8);
  fclose (input);
  memset (frame + core_size, 0x55, sizeof (frame) - core_size);

  assert (avformat_alloc_output_context2 (&mux, NULL, "spdif", NULL) >= 0);
  stream = avformat_new_stream (mux, NULL);
  assert (stream);
  stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  stream->codecpar->codec_id = AV_CODEC_ID_DTS;
  stream->codecpar->sample_rate = 48000;
  assert (av_opt_set_int (mux->priv_data, "dtshd_rate", 768000, 0) >= 0);
  assert (av_opt_set_int (mux->priv_data, "dtshd_fallback_time", 0, 0) >= 0);
  assert (av_opt_set (mux->priv_data, "spdif_flags", "be", 0) >= 0);
  assert (avio_open_dyn_buf (&mux->pb) >= 0);
  assert (avformat_write_header (mux, NULL) >= 0);
  packet->data = frame;
  packet->size = sizeof (frame);
  assert (av_write_frame (mux, packet) >= 0);
  burst_size = avio_close_dyn_buf (mux->pb, &burst);
  mux->pb = NULL;
  assert (burst_size >= 20);
  packed_size = burst[18] << 8 | burst[19];
  assert (packed_size == core_size && packed_size != packet->size);
  assert (memcmp (burst + 20, frame, core_size) == 0);
  printf ("ok - oversized frame reduced to %d bytes; length check detects fallback\n", packed_size);
  /* 1024 samples (period 16384) is accepted; 2048 (32768) is refused, which
   * gst_dtsdec_passthrough_decide avoids by not arming HD for it. */
  assert (write_core (31) >= 0);
  assert (write_core (63) < 0);
  printf ("ok - FFmpeg refuses a 2048-sample core at 768 kHz, accepts 1024\n");
  packet->data = NULL;
  packet->size = 0;
  av_packet_free (&packet);
  av_free (burst);
  avformat_free_context (mux);
  return 0;
}
