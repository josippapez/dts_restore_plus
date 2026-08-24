# DESIGN — discrete multichannel DTS for the C2/G2/CS profile

Status: **DRAFT / NOT IMPLEMENTED**. Scoped 2026-08-24 after the first working C2
install (`OLED55CS6LA`, firmware `23.25.55`, webOS `9.2.2`, GStreamer `1.18.5`)
reported DTS decoding correctly but arriving at the soundbar as **2.0 PCM**.

## Why it is stereo — settled, and it is our binary

The `webos22-o22-gst118` profile binds `gst/libgstlibav.so`, taken from
`lgstreamer/gst-libav@lg` (1.14.4). That build performs "DTS (dca) decode with
**forced stereo-integer downmix** and `[downmix]`-coefficient support"
([`../../README.md`](../../README.md) line 222; stated again at line 196). So the
profile can only ever produce 2.0, whatever the TV would accept.

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

## Order of work

Do [`DESIGN-C2-TS-DEMUX.md`](DESIGN-C2-TS-DEMUX.md) first: it restores playback of
whole containers, needs no patch, and needs no new source. This one only changes
2.0 to 5.1 on files that already play.

1. Read the channel count the sink actually receives, by the method in
   `../restore/dts2lpcm/retag-staging/RUNBOOK-hardware-verification.md` §3.6
   (mixer-input readout, known-stereo control first) — if that interface exists on
   webOS 9.2.2, which is unconfirmed. If the sink will not take 6 channels, stop.
2. Only then build.
