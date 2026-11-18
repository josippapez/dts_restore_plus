# MP4 TrueHD demux regression test

Run on Linux or WSL with Python 3, `qemu-arm`, FFmpeg with TrueHD, AC-3 and
MPEG-4 video encoders, and an ARM EABI soft-float sysroot. The runner uses the ARM
`gst-launch-1.0`, `gst-inspect-1.0` and core elements from LG GStreamer 1.24.
Use the SDK's `usr` directory or pass the prefix of a matching cross-build
with its tools enabled.

```sh
python3 webos25/restore/src/test/mp4-truehd/run-qemu.py \
  --sysroot /path/to/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot \
  --gst-prefix /path/to/lg-gstreamer
```

The runner defaults to `webos25/restore/demux-out/libgstisomp4.so`. Use
`--plugin` to check a candidate before replacing that binary. Each run uses
a temporary plugin directory, registry and synthetic media fixtures. The
only checked-in media it reads is the existing DTS-in-MP4 test clip.

The test compares the extracted TrueHD, AC-3 and DTS bytes with FFmpeg
stream-copy references. It verifies framed TrueHD caps, index compaction
and AC-3 track retention in both scheduling modes. The push path uses a
queue upstream of the demuxer. A 33 MiB `moov` must play in both modes.
A `moov` above 128 MiB must fail in both modes, and the push limit for
other atoms stays at 32 MiB.

Non-interleaved video and TrueHD fixtures check the chunk ranges used for
interleave detection and compare the extracted audio bytes with FFmpeg.
They cover 32-bit `stco` and 64-bit `co64` tables, with audio offsets above
4 GiB in a sparse file for the co64 case. Only the TrueHD track gets the
corrected range, so the co64 video track must keep stock qtdemux's empty
range and the file is not classified as non-interleaved.

The delayed sync fixture removes sample 1 from the TrueHD `stss` table.
The extra sync fixture adds an `stss` entry past the last sample, which the
compact index rejects. Both scheduling modes must use the ordinary index
for each and preserve the full TrueHD and AC-3 payloads.
