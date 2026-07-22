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

## Multichannel PCM — already delivered to the sink (BD-LPCM reframe NOT needed)

Update after on-device verification: LG's `audiosink` (`lgaudiosink`) advertises
`audio/x-raw … channels=[1,10]`, and a **real Media-Player playback** of a 5.1 DTS
file shows it negotiating `audio/x-raw, S32LE, 48000, channels=6` on its sink pad.
So the current `dtsdec → S32LE 5.1 → audiosink` path **already hands full discrete
5.1 PCM to LG's audio HAL** — there is no stereo downmix in the GStreamer path.

The **DTS → BluRay-LPCM reframe** (the `experimental/gst-dtstolpcm/` idea) was
premised on the sink being stereo-only; since the sink takes up to 10 channels
directly, that reframe is **unnecessary** to reach a multichannel sink. It would
only matter if LG's HAL gated eARC multichannel output specifically to the
proprietary `pcm_audiodec` path — which can be checked with an AVR (see below).

**The remaining variable is the TV's output stage, not the pipeline:** whether the
HAL renders the delivered 5.1 to eARC or folds it to the built-in speakers depends
on the "Digital Sound Output" setting + eARC EDID. Confirm on an AVR's input
display; if an AVR shows 2.0 while the pipeline delivers 6ch, the fold happens in
the closed HAL (not fixable from our layer) and only then is the `pcm_audiodec`
path worth attempting.

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
