# DTS / TrueHD bitstream passthrough on webOS 25 — feasibility (investigation)

**Question:** can we send an *undecoded* DTS or Dolby TrueHD bitstream to an AVR
(eARC/ARC/optical) — i.e. real passthrough — instead of software-decoding to PCM
(what this project does today)?

**Verdict: not achievable via any editable config, caps, or rank on webOS 25.**
The decode-vs-passthrough decision is compiled into LG's **closed audio HAL**, not
exposed in any file we can edit. (Investigated on a real C5, read-only, plus
GStreamer/LG-source/RootMyTV-#72 research.)

## Why

1. **LG's GStreamer audio sink takes PCM only.** The only sink is `lgaudiosink`
   (element name `audiosink`, `libgstlgaudiosink.so`); its sink pad accepts **only
   `audio/x-raw` (S8..S32 integer PCM)** — never `audio/x-dts`, `audio/x-ac3`,
   `audio/x-eac3`, or `audio/x-true-hd`. So a bitstream cannot even be expressed at
   the sink. No rank/caps change forces a sink to accept caps it doesn't advertise.
2. **Everything is decoded to PCM before the sink.** Even AC3 goes
   `ac3_audiodec (audio/x-ac3 → audio/x-raw)`. There is no `dcaparse`/`dtsparse` on
   the device, and no IEC61937-packing element in GStreamer to wrap a bitstream.
3. **`device_codec_capability_config.json` has no passthrough switch.** Entries are
   `{name, channels}` only (AC3/EAC3/DTS/DTSH/DTSE/TRUEHD/MLP) — a *decode*
   capability advertisement, not a bitstream flag. AC3 and DTS are identical in shape.
4. **Passthrough lives in the closed HAL.** `audiooutputd`'s
   `libelement-audio-decoder.so` (`Element::DecoderAudio`) tracks `codecSrc` vs
   `codecDecoded` and exposes `drvSetTPDecoderOutputMode` / `TPDecoderOutputMode`;
   `libelement-audio-ahdmi.so` (`Element::HdmiAudio`) drives the eARC/ARC output.
   Its `src_codec_ext_type_t` enum *does* include `dts_hd`, `dts_hd_ma`, `dts_x_p1/p2`,
   `dts_express`, `truehd`, `truehd_atmos`, `eac3_atmos`, `mat_atmos`, `ac4_atmos` —
   so the platform recognizes these as bitstream types — but the routing (decode vs
   bitstream) is decided at runtime from **source codec + eARC EDID + the user's
   "Digital Sound Output" (Auto/Pass-Through/PCM) setting**, inside compiled code and
   the SoC audio driver. There is no JSON/caps lever to flip.

## What IS potentially possible (future work — multichannel, not passthrough)

- **DTS → BluRay-LPCM reframe** (the `experimental/gst-dtstolpcm/` idea): LG's
  proprietary **multichannel** path stays reachable for the whitelisted
  `audio/x-private-ts-lpcm` (BD-LPCM) caps + `pcm_audiodec`. Decode DTS→PCM and
  re-frame as BD-LPCM so `decodebin` re-enters LG's autoplug onto the multichannel
  sink → real 5.1/7.1 **PCM to the speakers/eARC**. This is still decode-to-PCM (no
  bitstream), but it would upgrade beyond what the sink renders today. Highest-effort
  of the realistic options and unvalidated on hardware.

## Likely impossible without proprietary reverse-engineering

- True DTS or TrueHD **bitstream** over eARC via any open path.
- Rebuilding LG's closed `lgaudiosink` / `lxaudiodec` / `liblxa` / audio HAL.
- TrueHD passthrough specifically (no decoder, no sink caps, licensing).

## Read-only next steps to confirm (before any deeper attempt)

1. Play a DTS title and read `/tmp/gst.log` (gstcool sets `GST_DEBUG=5`) + `PmLog`
   for `TPDecoderOutputMode`, `codecSrc`/`codecDecoded`, and whether `audio-ahdmi`
   ever selects a bitstream mode — shows what the HAL chooses and why.
2. Read the AVR's advertised caps as the TV sees them via `com.webos.service.eim` /
   `com.webos.service.extinputs.eim` (EDID/eARC) — does the sink even signal DTS/TrueHD?
3. `strings` the SoC audio driver / `libAudioResources.so` for the
   `src_codec_ext_type_t` → output-mode mapping table — that table (or a driver
   ioctl), not a config file, is the true gate.

**Bottom line:** passthrough is out of reach on webOS 25 without proprietary RE.
Keep the honest "decode-to-PCM only, no passthrough" caveat in the README; the only
realistic upgrade is decode-then-reframe to multichannel PCM (BD-LPCM), not bitstream.
