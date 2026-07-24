# TrueHD / MLP decoder build notes

Unlike the DTS side (a patched gst-plugins-bad `dtsdec`, whose source is vendored
here as `gstdtsdec.c` / `gstdtsdec.h`), the TrueHD/MLP decoder set itself is a
plain, reproducible cross-build of **upstream** ffmpeg + gst-libav sources — no
patch is needed to get `avdec_truehd`/`avdec_mlp` registered at all:

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

## Make-up gain patch (loudness fix — NOT "no source patch" anymore)

`build-truehd.sh` now applies one inline source patch before `./configure`:
a make-up-gain patch to ffmpeg's `libavcodec/mlpdec.c` (`build-truehd.sh:64-257`,
generated to `/tmp/mlpdec-makeup-gain.patch` and applied with `git apply`,
falling back to `patch -p1`). It reads a user-tunable gain (dB) once at
`mlp_decode_init` from `/var/lib/webosbrew/truehd/gain.conf` (missing/invalid
file -> 0.0 dB unity, clamped to [-20, +20]), caches it as a linear multiplier
on `MLPDecodeContext`, and applies it to the packed PCM output in
`output_data()` with a saturating S32/S16 clamp. The patch adds only new,
uniquely-named static symbols confined to the mlp/truehd translation unit —
decoder registration, ABI, and the GLIBC ceiling below are unaffected, and
AAC/AC-3/E-AC-3/ALAC decoding in the same `libgstlibav.so` is untouched
(build-truehd.sh self-verifies this: `grep mlp_apply_makeup_gain`,
`build-truehd.sh:252-257`). See
[`../../docs/WEBOS25-DTS.md#loudness--make-up-gain`](../../docs/WEBOS25-DTS.md#loudness--make-up-gain)
for the full mechanism and [`../TUNING-RUNBOOK.md`](../TUNING-RUNBOOK.md) for
tuning + the rebuild/release loop this patch makes binary-affecting.

## Why S32LE matters (shared with DTS)

LG's `audiosink` accepts only integer PCM (S8..S32), no float. `avdec_truehd`
already emits native **S32** PCM, so it negotiates directly with the sink — the
same reason the DTS `dtsdec` was patched to output S32LE instead of F32LE.
