# TrueHD / MLP decoder build notes

Unlike the DTS side (a patched gst-plugins-bad `dtsdec`, whose source is vendored
here as `gstdtsdec.c` / `gstdtsdec.h`), the TrueHD/MLP support needs **no source
patch**. It is produced by a plain, reproducible cross-build of upstream sources:

- **ffmpeg n4.4.4**, configured minimally: only the `truehd` + `mlp` decoders,
  the `mlp` parser, and the `truehd` + `mlp` demuxers. Everything else is
  disabled (`--disable-everything`). Produces `libavcodec/util/format/filter` +
  `libswresample`.
- **gst-libav 1.18** (`meson` cross-build, with a direct-`gcc` fallback) linked
  against those ffmpeg libs. gst-libav registers `avdec_truehd` and `avdec_mlp`
  at runtime by enumerating libavcodec's decoders — the element names are
  generated, not static strings, so there is nothing to patch.

Why n4.4 + gst-libav 1.18: the TV's armel GStreamer dev headers are 1.18.4
(Debian bullseye), which pins the gst-libav source to 1.18; FFmpeg 5.0+ removed
deprecated APIs that gst-libav 1.18 still uses, so n4.4 is the known-good pairing
that still ships the TrueHD/MLP codecs.

## Target ABI (must match the C5 GStreamer userspace)

- 32-bit ARM, **EABI5 soft-float** (gnueabi/armel): `e_flags 0x05000200`,
  loader `/lib/ld-linux.so.3`.
- Every referenced GLIBC symbol version `<= GLIBC_2.35` (TV has glibc 2.35).
- Built inside `debian:11-slim` (glibc 2.31) on an arm64 host with
  `arm-linux-gnueabi-gcc`, `--disable-neon --disable-vfp --disable-asm` so no
  hard-float codegen can slip in.

## Reproduce

The full, commented recipe is `../build-truehd.sh` (and a copy travels with the
built artifacts in `../truehd-out/build-truehd.sh`). It writes the plugin +
ffmpeg libs and a `BUILD-REPORT.txt` (per-file `e_flags`, max GLIBC, TrueHD
decoder presence) to its `/out`. Those artifacts are checked into
`../truehd-out/` so `install.sh` can deploy them without a rebuild.

## Why S32LE matters (shared with DTS)

LG's `audiosink` accepts only integer PCM (S8..S32), no float. `avdec_truehd`
already emits native **S32** PCM, so it negotiates directly with the sink — the
same reason the DTS `dtsdec` was patched to output S32LE instead of F32LE.
