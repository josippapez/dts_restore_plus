#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/opt.h>

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
  packet->data = NULL;
  packet->size = 0;
  av_packet_free (&packet);
  av_free (burst);
  avformat_free_context (mux);
  return 0;
}
