# gst-dtstolpcm

A GStreamer plugin that restores **multichannel** DTS playback on LG webOS
TVs (OLED CX and similar) by decoding DTS to PCM and re-framing it as
**HDMV/Blu-ray LPCM** (`audio/x-private-ts-lpcm`).

It provides two elements:

- **`dtstolpcm`** — a decoder *bin* (klass `Codec/Decoder/Audio`, rank 310)
  with the internal chain
  `dcaparse ! avdec_dca ! audioconvert ! audioresample ! capsfilter(rate=48000) ! bdlpcmenc`.
  Sink caps `audio/x-dts` (+ `audio/x-private1-dts`), src caps
  `audio/x-private-ts-lpcm`.
- **`bdlpcmenc`** — the raw-PCM → BD-LPCM encoder used inside the bin
  (rank `NONE`; usable standalone for testing).

## The caps back-door (why this works)

LG's `decproxy`/`fakedec` proxy layer routes audio to a proprietary
**multichannel** decoder **only** for caps on an internal whitelist. LG
deleted every DTS cap from that whitelist, so open DTS decoders
(`avdec_dca`) are stuck feeding a nerfed **stereo** sink. However
`audio/x-private-ts-lpcm` (HDMV/Blu-ray LPCM) is **still whitelisted**, and
a proprietary LPCM decoder (`pcm_audiodec`, rank 290) is present and ready.
So this plugin decodes DTS → PCM and re-frames the PCM as BD-LPCM; because
the bin's *output* caps are non-raw and whitelisted, `decodebin` autoplugs
it into `decproxy` → the proprietary multichannel LPCM decoder → real
5.1/7.1 out. No proprietary library is reverse-engineered or modified.

A **bin** is mandatory: `decodebin` stops autoplugging once it reaches raw
`audio/x-raw`, so exposing raw PCM would dead-end. Keeping the public
output as `audio/x-private-ts-lpcm` makes the stream re-enter LG's autoplug
path.

## Wire format (BD-LPCM)

Per audio frame: **4-byte big-endian header + big-endian interleaved
samples**.

| Field | Bits/Bytes | Meaning |
|-------|-----------|---------|
| bytes 0-1 | BE u16 | payload size in bytes **excluding** the 4-byte header |
| byte 2 hi nibble | 4 bits | channel assignment: `3`=stereo(2ch), `9`=5.1(6ch), `11`=7.1(8ch) |
| byte 2 lo nibble | 4 bits | sample rate: `1`=48000, `4`=96000, `5`=192000 |
| byte 3 bits 7-6 | 2 bits | bits/sample: `1`=16-bit, `3`=24-bit (24-bit = 3 bytes/sample) |
| samples | — | interleaved, **big-endian**, **BD channel order with LFE LAST** |

BD channel order (differs from GStreamer SMPTE order, which puts LFE 4th —
the encoder reorders in software; `audioconvert` cannot express this):

- 5.1: `L, R, C, Ls, Rs, LFE`
- 7.1: `L, R, C, Lside, Ls, Rs, Rside, LFE` (confirmed against ffmpeg
  `pcm-bluray.c`)

Frame size: **240 samples/frame** for 16-bit, **360** for 24-bit (matches
`dvdlpcmdec` / `pcm_bluray`). The `GstAudioEncoder` base class accumulates
to those frame boundaries; the EOS tail is emitted as a short frame whose
size field reflects the real payload. **44100 Hz is invalid** for BD-LPCM,
so `audioresample` upstream converts it to 48000.

## Build (desktop, for testing)

```sh
meson setup build
ninja -C build
# load without installing:
GST_PLUGIN_PATH=$PWD/build gst-inspect-1.0 dtstolpcm
GST_PLUGIN_PATH=$PWD/build gst-inspect-1.0 bdlpcmenc
```

Requires `gstreamer-1.0`, `gstreamer-base-1.0`, `gstreamer-audio-1.0` dev
packages, plus `gst-plugins-good` (`dcaparse`, `audioconvert`,
`audioresample`) and `gst-libav` (`avdec_dca`) at runtime.

## Build (WebOSBrew ARM cross-compile)

The TV runs GStreamer **1.14.4**; the plugin must match that ABI. Use the
WebOSBrew starfish / `meta-lg-webos-ndk` toolchain targeting
**armv7a-neon** (hard-float):

```sh
source /opt/webos-sdk/environment-setup-armv7a-neon-webos-linux-gnueabi
meson setup build-arm --cross-file arm-webos.txt -Dprefix=/usr -Dlibdir=lib
ninja -C build-arm
# product: build-arm/libgstdtstolpcm.so
```

A minimal `arm-webos.txt` cross file points meson at the NDK's
`arm-*-gcc`/`pkg-config` and sets `host_machine` to `arm`/`linux`. Building
against a desktop 1.20+ base produces an **incompatible** `.so`.

## Install alongside dts_restore

1. Copy `libgstdtstolpcm.so` into the TV's GStreamer plugin directory
   (the same place `dts_restore` drops its rebuilt `libgst*.so`, typically
   under `/usr/lib/gstreamer-1.0/` or the path in `GST_PLUGIN_PATH` used by
   the media pipeline).
2. Ensure `dtstolpcm` outranks `avdec_dca`. This plugin already registers
   `dtstolpcm` at rank **310** (> `avdec_dca`'s 290), so `decodebin` picks
   it first. If your image pins ranks in `/etc/gst/gstcool.conf`, add/keep
   `avdec_dca` **below** 310 there (or leave it at its default 290) so it
   stays the fallback, not the primary.
3. Clear the plugin registry cache if elements do not appear:
   `rm -f ~/.cache/gstreamer-1.0/registry.*.bin` (adjust `HOME` on the TV).

`dts_restore` itself must remain installed: it re-enables DTS **demuxing**
in `libgstmatroska.so` / `libgstmpegtsdemux.so`. This plugin only changes
how the demuxed DTS is *decoded/routed*.

## Off-TV test plan (what CAN be validated on a desktop)

The byte layout and channel order are the risky, verifiable parts. Prove
them by round-tripping through GStreamer's own BD-LPCM decoder and ffmpeg's
`pcm_bluray`, comparing against the original PCM.

1. **Make a known 5.1 reference** (deterministic tones per channel):

   ```sh
   gst-launch-1.0 audiotestsrc num-buffers=100 ! \
     audio/x-raw,format=S16LE,rate=48000,channels=6 ! \
     audioconvert ! wavenc ! filesink location=ref_5p1.wav
   ```

2. **Encode with `bdlpcmenc`, decode with GStreamer `dvdlpcmdec` (BLURAY
   mode)** and confirm sample-exact recovery after undoing the BD reorder:

   ```sh
   GST_PLUGIN_PATH=$PWD/build gst-launch-1.0 \
     filesrc location=ref_5p1.wav ! wavparse ! audioconvert ! \
     audio/x-raw,format=S16LE,rate=48000,channels=6 ! \
     bdlpcmenc ! 'audio/x-private-ts-lpcm' ! \
     dvdlpcmdec ! audioconvert ! wavenc ! filesink location=rt_gst.wav
   ```

   `dvdlpcmdec` outputs in SMPTE order, so `rt_gst.wav` should match
   `ref_5p1.wav` channel-for-channel (allowing for the 240-sample framing
   at the tail). Compare with `ffmpeg -i ... -af astats` or a quick numpy
   diff.

3. **Cross-check byte layout with ffmpeg's `pcm_bluray`.** Dump the raw
   `bdlpcmenc` output (`filesink`) and feed it to ffmpeg with the
   `pcm_bluray` decoder (via a small container or `-f s16be`-style raw
   probe); confirm ffmpeg reports the expected channel count/order and
   decodes without header errors. Byte-compare the first 4 header bytes
   against the spec table above (e.g. 5.1@48k/16-bit → byte2 `0x91`
   = `(9<<4)|1`, byte3 `0x40` = `1<<6`).

4. **`gst-inspect-1.0 dtstolpcm`** should show klass `Codec/Decoder/Audio`,
   rank 310, sink `audio/x-dts`, src `audio/x-private-ts-lpcm`.

## Cheap on-TV pre-test (gates the whole approach)

Before trusting the autoplug path, prove the TV's proprietary LPCM decoder
actually emits multichannel: use **tsMuxeR** to mux a BD-LPCM 5.1 track
into a `.m2ts`, play it on the TV, and confirm the AVR/soundbar shows
**>2 channels**. If the `.m2ts` BD-LPCM path already gives real 5.1, the
`dtstolpcm` route is very likely to as well.

## UNTESTED here / hardware-only unknowns

This source was **not compiled or run** in the authoring environment (no
ARM toolchain, no TV). Specifically unverified:

- **Does `pcm_audiodec` actually output multichannel** for
  `audio/x-private-ts-lpcm`, or is it also capped to stereo? (Only the TV
  can answer — see the tsMuxeR pre-test.)
- **decproxy autoplug**: whether `decodebin`/`decproxy` on the TV really
  selects our non-raw output and routes it to the proprietary sink.
- **MKV/container resource policy**: LG's playback resource manager may
  treat MKV-carried DTS differently from `.m2ts`; the sink may refuse the
  reframed stream depending on container.
- **DTS-HD MA (XLL)**: `avdec_dca` typically decodes only the lossy **5.1
  core**, so 7.1/object extensions will down to 5.1 core; true 7.1 out is
  not guaranteed for HD-MA sources.
- **24-bit path** and **7.1 (8ch) reorder** are implemented to spec but
  not exercised against real hardware; the 5.1/16-bit path is the primary
  target.
- Compilation itself (warnings/1.14 symbol availability) is unverified —
  build with the desktop steps above first.
```
