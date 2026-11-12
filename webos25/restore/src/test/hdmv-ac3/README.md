# HDMV TrueHD and AC-3 demux regression test

Run from the repository root on Linux or WSL with Python 3, `qemu-arm`,
`pkg-config`, `ffprobe` and an ARM EABI soft-float toolchain. Use the libraries
and `libgstcoreelements.so` from the [`build-demux.sh`](../../../build-demux.sh)
cross-build of [LG webOS 25 GStreamer 1.24](https://github.com/lgstreamer/gstreamer-webos-25).
Upstream GStreamer lacks the LG-specific symbols used by this demuxer.

```sh
python3 webos25/restore/src/test/hdmv-ac3/run-qemu.py \
  --sdk /path/to/arm-webos-linux-gnueabi_sdk-buildroot \
  --gst-libdir /path/to/lg-gstreamer/lib
```

Use `--sysroot` if the QEMU loader and target system libraries are in a
separate ARM sysroot. The default plugin is
`webos25/restore/demux-out/libgstmpegtsdemux.so`. Use `--plugin` to test a
different build. The suite was verified in WSL with QEMU 8.2.2, FFprobe 6.1.1
and LG GStreamer 1.24.0.

### Run in the demux build container

`build-demux.sh` exports the demuxers and leaves its prepared source under
`TMPDIR`. To use the matching runtime, rebuild in a container that also runs
the probe before its `/opt/gst` installation is removed.

```sh
work=$(mktemp -d)
TMPDIR="$work" bash webos25/restore/build-demux.sh /path/to/gstreamer-webos-25 "$work/out"
sources=$(find "$work" -mindepth 2 -maxdepth 2 -type d -name src -print -quit)
docker run --rm -v "$sources:/src:ro" -v "$work/out:/out" -v "$PWD:/repo:ro" \
  -w /repo demux-armel bash -ec '
    apt-get -o Acquire::Check-Valid-Until=false update
    apt-get install -y --no-install-recommends qemu-user ffmpeg
    /build-inside.sh
    python3 webos25/restore/src/test/hdmv-ac3/run-qemu.py \
      --cc arm-linux-gnueabi-gcc --sysroot / --gst-libdir /opt/gst/lib \
      --plugin /out/libgstmpegtsdemux.so
  '
```

### Fixtures

The runner loads the chosen plugin in an isolated GStreamer registry, prints
its SHA-256, and checks the actual `GstStreamCollection` plus the bytes emitted
by `tsdemux`.

| Fixture | Expected result |
| --- | --- |
| `with-ac3.m2ts` | TrueHD and AC-3 on PID `0x1100`, followed by AC-3 on `0x1101` and `0x1102`, in FFprobe order. The companion caps are `audio/x-ac3` and each output matches its elementary stream byte for byte. |
| `no-ac3.m2ts` | Same collection and caps. TrueHD still uses PES extension `0x72`, while the AC-3 companion stays empty and the later tracks keep their indices and payloads. |
| `extensionless-truehd.m2ts` | Same TrueHD bytes and empty AC-3 slot with `private_stream_1` (`0xbd`) PES and no PES extension. |
| `non-hdmv.m2ts` | The PMT registration is `TEST` instead of `HDMV`. Both ordinary AC-3 tracks still demux and the companion is absent. |

The fixtures contain 320 ms of generated sine waves. The generator muxes
TrueHD (`0x72`) and the AC-3 companion (`0x76`) into the same physical type
`0x83` PID and writes the original encoded streams as byte references. The
type `0x83` entry has no AC-3 descriptor, so the test also checks that the
companion gets AC-3 caps independently of the shared PMT entry.

The committed fixtures are ready to use. To regenerate them with an FFmpeg
build that includes the TrueHD and AC-3 encoders, run the following command.
The original fixtures were generated with FFmpeg 8.0.1.

```sh
python3 webos25/restore/src/test/hdmv-ac3/make-fixtures.py
```
