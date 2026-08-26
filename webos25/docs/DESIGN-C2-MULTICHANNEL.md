# DESIGN — discrete multichannel DTS for the C2/G2/CS profile

Status: **DRAFT / NOT IMPLEMENTED**. Scoped 2026-08-24 after the first working C2
install (`OLED55CS6LA`, firmware `23.25.55`, webOS `9.2.2`, GStreamer `1.18.5`)
reported DTS decoding correctly but arriving at the soundbar as **2.0 PCM**.

## Why it is stereo — NOT settled; three live suspects

The `webos22-o22-gst118` profile binds `gst/libgstlibav.so`, taken from
`lgstreamer/gst-libav@lg` (1.14.4), described as "DTS (dca) decode with **forced
stereo-integer downmix** and `[downmix]`-coefficient support"
([`../../README.md`](../../README.md) line 222; again at line 196).

**That description is not the whole story, and an earlier draft of this doc wrongly
called the cause settled.** This repo's own evidence contradicts "can only ever
produce 2.0": [`FIRMWARE-COMPATIBILITY.md`](FIRMWARE-COMPATIBILITY.md) lines 216-218
record the *same* binary decoding to **six-channel S32LE** under QEMU on a stock
1.18.2 rootfs, and lines 333-334 record it again for B2. Disassembly agrees the
downmix is **conditional**: `ff_dca_downmix_to_stereo_forced` (an LG symbol absent
from upstream) is reached through a guard, so it is not applied unconditionally.

Three suspects remain live, and the fix differs for each:

1. **The decoder**, if the LG patch downmixes on the TV but not under QEMU.
2. **Downstream fold** in LG's 1.18 `audiosink`/HAL/`audioconvert`. The C2 sink's
   caps have never been read; the `channels=[1,10]` reading in
   [`PASSTHROUGH.md`](PASSTHROUGH.md) lines 38-40 is **C5-only**.
3. **The HDMI link.** Plain ARC and optical carry at most 2-channel LPCM;
   multichannel LPCM needs **eARC** plus the right "Digital Sound Output" setting
   (the same variable `PASSTHROUGH.md` lines 46-50 records for the C5). If the
   owner's soundbar is on ARC or optical, "stereo PCM at the soundbar" is expected
   **no matter what any decoder emits**, and no build can change it.

Suspect 3 is the cheapest to rule out and was missing from this doc entirely.

Two things this is **not**:

- **Not the codec capability config.** `device_codec_capability_config.json` entries
  are `{name, channels}` only — a decode-capability advertisement with no
  passthrough switch ([`PASSTHROUGH.md`](PASSTHROUGH.md) line 22). On the C5 it
  still lists `DTS`/`DTSE`/`DTSH` after LG removed the decoder (line 96), so it
  advertises rather than gates.
- **Not `downmix.conf`.** That file tunes the mix coefficients
  (`front`/`center`/`lfe`/`rear`/`rear2`), never the channel count. In LG's source
  the coefficients are pushed into `context->downmix.*`
  (`ext/libav/gstavauddec.c:309-315` on the `lg` branch); the downmix itself happens
  inside LG's patched libavcodec dca decoder.

The webOS 25 build already proves the target is reachable: it "does **not** downmix
— it decodes native discrete 5.1 (verified on a C5)" (`../../README.md` line 145).

## Mechanism

Ship a **second, separate** `libgstlibav.so` for the C2 family, built for GStreamer
1.18 against **upstream** ffmpeg, with `dca` enabled and none of LG's downmix code.
Discrete output is upstream ffmpeg's default for DTS.

Why upstream ffmpeg rather than LG's: the `downmix` struct is an LG addition to
`AVCodecContext`, and LG's ffmpeg/libav fork is **not published** — the `lgstreamer`
org holds `gstreamer`, `gst-plugins-{base,good,bad,cool}`, `gst-libav` (all 1.14.4)
and `gstreamer-webos-25` (1.24.0), and nothing else. Building LG's gst-libav against
upstream ffmpeg would fail on the missing `downmix` member, so those lines
(`gstavauddec.c:246-284`, `:309-315`) are removed or the upstream plugin is used.

### Why a 1.18 build, and why it will load

`gst_plugin_check_version()` rejects a plugin only when
`major != GST_VERSION_MAJOR || minor > GST_VERSION_MINOR`
(`gstplugin.c:487` in the webOS-25 tree). A plugin built for 1.18.4 therefore loads
on a 1.18.5 core, and the existing 1.14.4 payload loading on that TV today is the
same rule in action. **No LG webOS-22 source is required**, which is fortunate:
none is published.

Debian **bullseye ships GStreamer 1.18.4** and still has `armel` in `bullseye/main`,
and `build-demux.sh` already builds inside `debian:11-slim` for exactly that ABI
(`ld-linux.so.3`, ELF32 ARM EABI5 soft-float, `e_flags 0x05000200`). The toolchain
is a closer fit here than it was for the 1.24 work.

### Build

Adapt `build-truehd.sh` (it already builds a `libgstlibav.so` with statically linked
ffmpeg) to target 1.18.4:

- base `debian:11-slim`, `SNAPSHOT=20250601T000000Z` (the pin `build-demux.sh` uses,
  the last snapshot with armel in bullseye/main);
- GStreamer/`gst-plugins-base` 1.18.4 headers for the plugin ABI;
- ffmpeg configured with the DTS decoder enabled and **no** downmix patch;
- output kept out of the existing directories: a new `libav-118-out/`, so the
  committed 1.24 artifacts are untouched.

### Deployment

A separate payload directory per target, selected by profile, rather than
overloading `payload/cx/`:

- `payload/c2-multichannel/libgstlibav.so` — the new 1.18 build;
- the C2 engine binds it instead of `payload/cx/libgstlibav.so` when the profile
  resolves to `webos22-o22-gst118`;
- `payload/cx/` stays exactly as-is for `cx-armv7-gst114`, whose forced 2.0 downmix
  is correct for that generation.

Keep it behind its own switch, defaulting off, so a failure falls back to today's
working stereo path rather than breaking a profile that now demonstrably works.

## Unverified — do not state these as facts

1. **Does the C2 sink accept 6-channel PCM?** Unknown. The C5's own multichannel
   evidence is a single `/proc` reading with no listening test, so it is not
   transferable. This is the risk that could make the whole build pointless, and it
   is the first thing to measure.
2. **Loudness.** A decoder without LG's downmix also has no dialnorm or DRC, so
   output may be quiet — the same problem `MULTI-MODEL.md` §2.6 records for C1/C2.
3. **DTS-HD MA vs core.** Upstream ffmpeg decodes the DTS core of an MA track; it is
   not a lossless MA decode.
4. ~~**Does the failing playback even reach our decoder?**~~ **Answered
   2026-08-24: multichannel is not the playback blocker.** All three bundled samples
   are 6-channel DTS-HD MA by `ffprobe`, and `DTS-in-mp4.mp4` plays on the C2 while
   the `.ts`/`.m2ts` do not. A 5.1 file therefore already plays; what is missing is
   the TS container, not channel support. See
   [`DESIGN-C2-TS-DEMUX.md`](DESIGN-C2-TS-DEMUX.md).

   This work is therefore **output quality (2.0 → 5.1), not playability**, and ranks
   below the TS demuxer.

## The CS/C2 hardware can apparently decode DTS itself (2026-08-25)

Read from the owner's own image, `23.25.55.01-HE_DTV_W22O_AFABATPU`. This reframes the
whole problem and came from the owner's question "why not use C3's GStreamer files?".

| layer | CS/C2 state | evidence |
|---|---|---|
| audio DSP firmware | **`dec_dtsx` present** | `lib/firmware/audio_a0_dsp0.bin` strings; siblings `dec_ddp`, `dec_mat`, `dec_ac4`, `dec_mpegh`; **no** plain `dec_dts` |
| audio-adapter shim | `usr/lib/aa/libaa_dtsx.so` (10 KB) — `dtsx_src`, `mod_dtsxsrc`, `DTSHDHDR` | a source/parser hook to the DSP, far too small to be a codec |
| codec capability | `DTS`, `DTSH`, `DTSE` all `"channels": 6` | `etc/umediaserver/device_codec_capability_config.json` |
| element rank | `dts_audiodec=290` | `etc/gst/gstcool.conf` (alongside `avdec_dca=0`) |
| the GStreamer element | **absent** — no `dts_audiodec`, `libgstdtsdec`, or `libdca` anywhere in the image | rootfs listing |

So on this generation the DSP decoder, the adapter shim, a 6-channel capability
declaration and a rank entry are **all present**, and the only missing link is the
userspace GStreamer element that would route `audio/x-dts` into that path. The
`dts_audiodec=290` line is not a meaningless leftover after all: it is LG's own
rank for an element this image no longer ships.

If that element can be supplied — the owner's suggestion was to take it from a C3,
which runs the same GStreamer 1.18.x — DTS would decode **in hardware, multichannel**,
instead of through our software `avdec_dca` with its forced stereo downmix. That is a
better outcome than any of the build options below, and it may also restore the DTS
badge.

**Unverified, and each could kill it:**

1. A `dec_dtsx` string in a firmware blob is not proof the module is licensed or
   reachable at runtime. DSP blobs are often shared across models regardless of
   entitlement, and LG may have removed the element precisely because this model is
   not licensed.
2. C3's `dts_audiodec` is presumably a thin wrapper over LG's audio-adapter API; its
   ABI must match this image's `libmodule_decoder.so` / `libaa_*` generation, not just
   the GStreamer minor version.
3. No C3 firmware has been extracted yet. That is the next step, and it is the same
   procedure `DESIGN-C2-TS-DEMUX.md` documents.
4. `dec_dtsx` is DTS:X; whether it also accepts DTS core and DTS-HD MA is an
   assumption about DTS:X decoders being supersets, not something measured here.

**This now outranks the gst-libav rebuild below.** Extract a C3 image, confirm
`dts_audiodec` exists and what it links against, then decide.

## Measure before building

All three measurements are cheaper than one build, and any of them can make the
build pointless:

1. **Ask the owner:** eARC or plain ARC/optical, and the Digital Sound Output
   setting. Free, and rules out suspect 3.
2. **Read the C2 audio sink's caps template** from the already-extracted
   `23.25.55.01-HE_DTV_W22O_AFABATPU` rootfs. Free and local; never done for C2.
3. **Capture `avdec_dca`'s negotiated src caps** during real 5.1 playback on the
   owner's TV. One round-trip.

Only if the link is eARC-capable, the sink takes multichannel, and `avdec_dca`
negotiates 2 channels is the decoder the culprit.

## A second build option this doc originally missed

Retarget **this project's own `gstdtsdec` + libdca** to 1.18, rather than building
gst-libav against upstream ffmpeg. It swaps one decoder element instead of the whole
libav plugin (which carries every `avdec_*` on the TV, including the AC3/AAC paths
that work today), and it keeps the make-up-gain/DRC work, avoiding the loudness
regression an upstream dca would bring. Costs more up front; better to live with.

Note also that building a 1.18 payload contradicts the recorded decision in
[`MULTI-MODEL.md`](MULTI-MODEL.md) lines 208-209. Amend that explicitly rather than
silently overriding it.

## Order of work

Do [`DESIGN-C2-TS-DEMUX.md`](DESIGN-C2-TS-DEMUX.md) first: it restores playback of
whole containers, needs no patch, and needs no new source. This one only changes
2.0 to 5.1 on files that already play.

1. Read the channel count the sink actually receives, by the method in
   `../restore/dts2lpcm/retag-staging/RUNBOOK-hardware-verification.md` §3.6
   (mixer-input readout, known-stereo control first) — if that interface exists on
   webOS 9.2.2, which is unconfirmed. If the sink will not take 6 channels, stop.
2. Only then build.
