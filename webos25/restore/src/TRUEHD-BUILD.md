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

## Make-up gain + DRC patch (loudness fix — NOT "no source patch" anymore)

`build-truehd.sh` now applies one inline source patch before `./configure` to
ffmpeg's `libavcodec/mlpdec.c` — and it is no longer "make-up gain" alone:
the patch also ports a full DRC compressor (mode off/line/rf + boost%/cut%)
and a separate centre-channel dialogue boost, mirroring the DTS side
byte-for-byte (see [`../../docs/WEBOS25-DTS.md#loudness--make-up-gain`](../../docs/WEBOS25-DTS.md#loudness--make-up-gain),
"Dynamic range compression (DRC) + dialogue boost", for the DSP model and the
LG evidence behind it). The patch is generated to
`/tmp/mlpdec-webos25-loudness.patch` and applied with `git apply`, falling
back to `patch -p1`. Before applying, `build-truehd.sh` asserts a **pre-apply
scope guard** on the patch text itself: every `+++`/`---` header must name
`libavcodec/mlpdec.c` and nothing else, and there must be exactly 2 such
headers — so an empty or truncated heredoc cannot silently read as
"no out-of-scope files" (`build-truehd.sh:1268-1283`). After applying, it
runs **8 verify checks** against the mutated file (`build-truehd.sh:1294-1321`):
6 presence checks (`mlp_apply_makeup_gain`, the truehd `gain.conf` path, the
ported `<<<DRC-CORE-BEGIN>>>` block, the DRC curve function, the per-sample
DRC apply, the level detector), one asserting the retired silence gate has
NOT been reintroduced, and one asserting the detector reads samples before
any gain is applied (feed-forward, required because the DRC curve is
unimodal — a post-gain detector could limit-cycle on the decay leg).

At runtime: `mlp_decode_init` reads all of gain/DRC/centre-boost once from
`/var/lib/webosbrew/truehd/gain.conf` (missing/invalid → each key's own
inert default), caches linear multipliers on `MLPDecodeContext`, and
`output_data()` applies them to the packed PCM output — DRC gain identical
across channels, centre boost per-channel, make-up gain last — with the
existing saturating S32/S16 clamp. The patch adds only new, uniquely-named
static symbols confined to the mlp/truehd translation unit — decoder
registration, ABI, and the GLIBC ceiling below are unaffected, and
AAC/AC-3/E-AC-3/ALAC decoding in the same `libgstlibav.so` is untouched (the
scope guard above proves this mechanically, before the patch is even
applied).

**DTS↔TrueHD core-drift invariant:** the DRC math itself — the block
detector, the curve, the boost/cut scaling, the smoothing — is a
byte-for-byte copy of the `<<<DRC-CORE-BEGIN>>>`/`<<<DRC-CORE-END>>>` block
in `../src/gstdtsdec.c`. `src/test/run-tests.sh` enforces this: it extracts
both cores and diffs them, **failing the build if they have drifted apart**,
so a change to the DSP math in one decoder cannot silently go unported to
the other. `run-tests.sh` covers **both** decoders — the shared core via the
DTS extraction, plus TrueHD-specific host-binding tests (windowed
accumulation across MLP access units, patch-hunk arithmetic) — and is the
gate to run **before** any cross-build; see
[`../TUNING-RUNBOOK.md`](../TUNING-RUNBOOK.md) for the full
test → rebuild → verify → recommit → tag loop this patch makes
binary-affecting.

## Why S32LE matters (shared with DTS)

LG's `audiosink` accepts only integer PCM (S8..S32), no float. `avdec_truehd`
already emits native **S32** PCM, so it negotiates directly with the sink — the
same reason the DTS `dtsdec` was patched to output S32LE instead of F32LE.

## Buffer-rate patch (0.5 s dropout fix, 2026-09-01)

`build-truehd.sh` applies a second inline patch, to gst-libav's
`ext/libav/gstavauddec.c`: `avdec_truehd`/`avdec_mlp` default the
GstAudioDecoder `min-latency` property to 40 ms. TrueHD access units are 40
samples (0.83 ms), so an unaggregated decoder emits 1200 output buffers/s;
LG's audio renderer audibly drops ~0.5 s chunks at that rate whenever the
video track sustains ~40 Mbps (measured on a C5 with a 38.7 Mbps remux — the
identical file's AC-3 track at ~31 buffers/s played clean). With 40 ms
aggregation the sink sees ~25 buffers/s and the dropouts are gone; decoded
PCM is bit-identical (md5-verified on-device). The property stays
per-instance overridable; every other avdec keeps stock behavior.
