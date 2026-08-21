# Restoring DTS on webOS 25 (C5 / G5 and other GStreamer 1.24 sets)

> **SUPERSEDED — historical investigation note (2026).** The statements below
> were written during the initial webOS-25 investigation and contain a **wrong ABI
> assumption**: webOS 25 GStreamer userspace is **not aarch64**. Verified on-device, the C5's
> GStreamer userspace is **32-bit ARM EABI5 soft-float (`armel`)**, GStreamer
> 1.24.0, on an aarch64 *kernel* (see [`../README.md`](../README.md) and
> [`MULTI-MODEL.md`](MULTI-MODEL.md)). The aarch64 toolchain/build instructions and
> the "bottom line" below are therefore obsolete; the working C5 build is the
> Debian-armel 1.22 cross described in `MULTI-MODEL.md §2.2`, and the TS-coverage
> and TrueHD sections below describe the actual shipped payload. Treat this page as
> history, not as current instructions.

**The legacy `dts_restore` binaries in the repository root do NOT work on webOS
25.** They are GStreamer **1.14.4 / ELF32 ARM EABI5 soft-float (OLED CX)**
libraries; webOS 25 TVs such as the C5 run a **GStreamer 1.24 armel userspace on
an aarch64 kernel**. The plugin ABI and restore mechanism are incompatible;
bind-mounting the 1.14 libraries **breaks** MKV/MP4 playback instead of adding
DTS.

This document describes what webOS 25 actually needs.

## How DTS is disabled on webOS 25 (verified on a rooted C5)

Unlike CX — where LG stripped DTS demuxing out of `libgstmatroska.so` — webOS 25
keeps the demuxer intact and simply **ships no DTS decoder**:

| Check (on-device) | Result |
|---|---|
| `strings libgstmatroska.so \| grep -i dts` | `audio/x-dts`, `A_DTS` present → **demux works** |
| `gst-inspect-1.0 dts_audiodec` | `No such element or plugin` → proprietary DTS decoder **absent** |
| `gst-inspect-1.0 avdec_dca` / `dcaparse` | not registered → open-source decoder **absent** |
| `gstcool.conf` `[rank]` | still lists `dts_audiodec=290` (a rank for a decoder that isn't installed) |

So the Matroska/TS demuxer will happily produce `audio/x-dts`, but there is
**nothing downstream to decode it**. The fix is therefore to **supply a
GStreamer-1.24 DTS decoder for aarch64** and rank it — no demuxer patch needed.

## What to build

Two viable decoders (build ONE):

1. **`dtsdec` (gst-plugins-bad + libdca)** — lightest. A dedicated DTS decoder
   using `libdca`. Fewer moving parts than ffmpeg. GPL-2.0 (libdca).
2. **`avdec_dca` (gst-libav + ffmpeg)** — what the CX build used. Heavier
   (pulls ffmpeg), but you can reuse this repo's CX gst-libav patches
   (force stereo-integer downmix, `[downmix]` coefficients).

> **Status: background note from the initial investigation.** The shipping solution and current,
> accurate status are in [`../README.md`](../README.md) — the C5 is 32-bit **soft-float armel**
> (not aarch64 as early drafts assumed), and the shipped `dtsdec` emits **S32LE, up to 5.1** (LG's
> sink is integer-only). Whether the TV renders full surround at the output vs downmixes to stereo
> is the remaining open question; bitstream passthrough needs the proprietary sink path (see
> `experimental/` for the DTS→BluRay-LPCM converter sketch).

## Toolchain

You need an **aarch64 webOS-25 cross toolchain**, not the CX armv7 SDK:

- Source tree: [`lgstreamer/gstreamer-webos-25`](https://github.com/lgstreamer/gstreamer-webos-25)
  — GStreamer **1.24** Meson monorepo from LG's "webOS 25 WG_2.0" GPL drop; its
  `build.sh`/`README` reference the **starfish 9.0.0** toolchain + meson 1.4.0.
- Its `subprojects/gst-plugins-bad/meson_options.txt` exposes `dca`
  (`option('dca', type:'boolean', value:true)`) and
  `subprojects/gst-libav` builds `avdec_dca`. In the public GPL drop these
  default **on**; LG ships them off via build flags — so a clean build with the
  option enabled produces a DTS-capable plugin.

Build sketch (adapt to the actual toolchain env):

```sh
# with the aarch64 webOS-25 cross env sourced (starfish 9.0.x)
git clone https://github.com/lgstreamer/gstreamer-webos-25
cd gstreamer-webos-25
meson setup build --cross-file <aarch64-webos.txt> \
  -Dbad=enabled -Dlibav=enabled -Ddca=true         # dtsdec and/or avdec_dca
ninja -C build
# outputs: build/subprojects/gst-plugins-bad/ext/dts/libgstdtsdec.so
#     and/or build/subprojects/gst-libav/ext/libav/libgstlibav.so  (aarch64/1.24)
```

Verify the ELF before deploying: `file libgstdtsdec.so` → `ELF 64-bit LSB … ARM aarch64`.

## Install on the TV (rooted)

The C5 is already rooted (faultmanager). Deploy WITHOUT touching firmware —
same overlay approach as the CX tool, but a decoder plugin rather than a demuxer:

```sh
# 1. copy the built plugin somewhere persistent under webosbrew
mkdir -p /var/lib/webosbrew/dts25
cp libgstdtsdec.so /var/lib/webosbrew/dts25/          # (or libgstlibav.so)

# 2. bind-mount it into the plugin dir (add to a boot init.d script for persistence)
mount -n --bind -o ro /var/lib/webosbrew/dts25/libgstdtsdec.so \
      /usr/lib/gstreamer-1.0/libgstdtsdec.so

# 3. rank the DTS decoder so decodebin autoplugs it for audio/x-dts.
#    webOS 25 gstcool.conf uses a [rank] section (e.g. aac_audiodec=290);
#    add the element name your build registers, e.g.:  dtsdec=290   (or avdec_dca=290)

# 4. refresh the GStreamer registry so the new plugin/rank is picked up
export GST_REGISTRY_1_0=/tmp/gst_1_0_registry.arm.bin
gst-inspect-1.0 >/dev/null
# (bind the regenerated registry over the real one, as init_dts.sh does)
```

Then `gst-inspect-1.0 dtsdec` (or `avdec_dca`) should list the element, and a
DTS MKV should play in stereo.

## Open unknowns (need on-device iteration)

- Whether webOS 25's `decproxy`/`fakedec` caps route `audio/x-dts` to a software
  decoder at all, or gate it the way CX gated multichannel (the `dts_seamless`
  strings in `libgstdecproxy.so` suggest DTS is referenced but decoder-less).
- Multichannel: same wall as CX — only the proprietary sink does >2.0. The
  `gst-dtstolpcm/` LPCM-converter path is the theoretical route, rebuilt for 1.24.
- Whether a prebuilt community aarch64/1.24 `dtsdec` exists (check webosbrew).

**Bottom line:** DTS on the C5 is achievable but requires an aarch64 / GStreamer
1.24 decoder cross-build — it cannot be done with the CX binaries in this repo,
and it cannot be produced without the webOS-25 toolchain.

## TS container coverage — which `.ts`/`.m2ts` files actually work

`build-demux.sh` flips one default: `demux->dts_support` FALSE → TRUE
(`tsdemux.c:1180`). That gate wraps **five** DTS recognition sites, and which one a
file lands in is decided entirely by its PMT:

| site | condition | result |
|---|---|---|
| `tsdemux.c:3058` | **HDMV programs only** (`program->registration_id == DRF_ID_HDMV`), stream_type 0x82/0x85/0x86 | `audio/x-dts`, **sets `target_pes_substream = 0x71`** |
| `tsdemux.c:3174` | DVB extension descriptor, DTS-UHD, decoder profile 0 | `audio/x-dtsx` |
| `tsdemux.c:3195` | `GST_MTS_DESC_DVB_DTS` descriptor (0x7B) | `audio/x-dts` |
| `tsdemux.c:3248` | per-stream registration id `DTS1`/`DTS2`/`DTS3`/`DTSH` | `set_caps_for_private_dts()` |
| `tsdemux.c:3818` | `case ST_PS_AUDIO_DTS` — stream_type **0x8A** | `audio/x-dts` |

Note there is **no bare stream_type path for 0x82 outside an HDMV program**, by
design (see the SCTE warning below). Upstream GStreamer 1.24 has no `dts_support`
field at all — the gate is LG's; the recognition matrix is upstream's, plus LG's
additions (0x82 in the HDMV branch, `DTSH`, and the DVB-DTS/DTS-UHD descriptor
paths). Flipping the gate therefore yields **upstream coverage or better**, never
less.

Measured on a rooted C5 with the self-test pipeline
(`filesrc ! tsdemux name=d d. ! queue ! dtsdec ! audioconvert ! wavenc`):

| how the file was muxed | PMT | result |
|---|---|---|
| BluRay disc / tsMuxeR (`restore/testfiles/*.ts`) | HDMV + 0x86 + real 0x71 substream | ✅ 4,230,548 B |
| `ffmpeg -f mpegts` (default) | 0x82, no HDMV registration | ❌ no audio pad, `not-linked` |
| ffmpeg, PMT hand-retyped 0x82 → 0x86 | 0x86, no HDMV registration | ❌ still `not-linked` |
| `ffmpeg -f mpegts -mpegts_m2ts_mode 1` | **HDMV** + 0x82, no substream header | ⚠️ recognised, **44-byte WAV — silent** |
| ffmpeg, PMT hand-retyped 0x82 → **0x8A** | 0x8A | ✅ 4,228,268 B |
| DVB broadcast carrying descriptor 0x7B | 0x06 + DVB DTS descriptor | ✅ per code; untested (no source) |

Two failure modes, two distinct causes:

1. **Default ffmpeg TS gets no audio pad at all.** 0x82/0x85/0x86 are consulted
   *only* inside the HDMV branch, so without the program's HDMV registration
   descriptor nothing claims the stream. This is why retyping to 0x86 does not
   help — the stream_type is irrelevant until the program is HDMV.
2. **`-mpegts_m2ts_mode 1` is recognised but silent.** It *does* enter the HDMV
   branch, which then sets `target_pes_substream = 0x71`; ffmpeg writes no BluRay
   PES substream header, so every payload is filtered out and you get a bare WAV
   header. This is the worst case — it looks supported and plays nothing.

**Do NOT "fix" this by adding a non-HDMV 0x82 case.** In LG's own tree,
`gst-plugins-bad/gst-libs/gst/mpegts/gst-scte-section.h:56,59`:

```c
GST_MPEGTS_STREAM_TYPE_SCTE_SUBTITLING = 0x82,   /* Subtitling data */
GST_MPEGTS_STREAM_TYPE_SCTE_SIT        = 0x86,   /* Splice Information Table */
```

Outside HDMV those values are SCTE-27 subtitling and SCTE-35 splice information
(ad-insertion signalling, ubiquitous in cable/OTT). Claiming them globally would
route subtitle and splice PIDs into `dtsdec` on live TV — which is exactly why
upstream confines them to HDMV programs. If bare-0x82 DTS files ever show up in
real user reports, the defensible fix is **content-based**: peek the PES payload
for the DTS sync word (`0x7FFE8001` and its variants) before typing an otherwise
unclaimed private stream as DTS. That is real demuxer logic, not a default flip,
and like any demuxer change it is **binary-affecting** — rebuild, verify on a real
webOS-25 TV, re-commit the `.so` in the same change (see
[`.claude/rules/releasing.md`](../../.claude/rules/releasing.md)).

**User-facing guidance:** MKV and MP4 work. For `.ts`/`.m2ts` use tsMuxeR or a
straight disc copy. Avoid `ffmpeg -mpegts_m2ts_mode 1` for DTS (silent), and if
you only have an ffmpeg-muxed `.ts`, remux to MKV with `-c copy`.

**The ffmpeg-mux warning applies to TrueHD too, with a different symptom.** Measured
2026-08-19: an ffmpeg-muxed BDAV m2ts carrying TrueHD Atmos 7.1 *is* recognised —
LG's demuxer exposes `audio: Dolby TrueHD` — but it reports **2 channels** instead of
8 and decodes to **nothing**. ffmpeg does not write the BD PES substream framing that
the HDMV TrueHD path's `target_pes_substream = 0x72` selects, so the substream never
resolves. Same conclusion as DTS: use tsMuxeR or a disc copy, never an ffmpeg mux.

## TrueHD in `.ts`/`.m2ts` — a separate gate, and a silent AC-3 substitution

DTS was not the only codec LG switched off in `tsdemux.c`. The BluRay TrueHD
stream-type case is wrapped in `#if 0` and falls through to `goto done`:

| site | condition | what LG does |
|------|-----------|--------------|
| `tsdemux.c:3035` | HDMV programs, stream_type **0x83** (`ST_BD_AUDIO_AC3_TRUE_HD`) | `#if 0` → `goto done` — pad **never exposed** |

The failure mode is worse than silence, and it is why this went unnoticed: a BD
TrueHD track carries an **AC-3 compatibility substream on the same PID**, so with
the TrueHD pad suppressed the AC-3 core is what decodes. Playback sounds
completely normal while not being TrueHD at all — the "TrueHD plays fine" trap.
Measured on a real C5 before the fix, a BD m2ts with TrueHD 5.1 + 4× AC-3 exposed
**only** `audio #2/#3/#4: AC-3`; `tsdemux` advertised no TrueHD/MLP caps at all
and `strings` on the shipped `libgstmpegtsdemux.so` contained no `audio/x-true-hd`.

LG's own comment gates the case on *"until we have ability to decode this
codec"* — a precondition this project satisfies, since it ships `avdec_truehd`
(ranked 310 by `install.sh`). So `build-demux.sh` un-`#if-0`s it. The
`stream->target_pes_substream = 0x72` inside the case is load-bearing: it selects
the TrueHD PES substream rather than the embedded AC-3 core (compare DTS's `0x71`
at `tsdemux.c:3058`).

After the fix, the same file reports `audio #2: Dolby TrueHD, Channels: 6
(FL FR FC LFE SL SR)` and decodes to `audio/x-raw, S32LE, 48000, channels=6,
channel-mask=0x0c0f`. **The channel mask is the proof:** TrueHD decodes with the
**side** pair (`0x0c0f`), AC-3 on this content with the **rear** pair (`0x003f`),
so `0x0c0f` means the TrueHD substream genuinely reached the decoder. DTS in both
`.ts` (188-byte) and `.m2ts` (192-byte BDAV) re-verified unchanged.

**`.mp4` TrueHD is still unsupported** — and unlike the TS case this is not a gate
to flip: `qtdemux.c` has no TrueHD/MLP codepath at all (no `mlpa` fourcc
handling), so it needs new demuxer logic. Use MKV or `.m2ts` for TrueHD.

## Loudness / make-up gain

> **Scope: webOS 25 only, deliberately.** This whole feature exists because *our*
> software decoders apply no loudness management while LG's native decoders do. It
> therefore only applies where LG's decoder is gone. On C3/C4-era sets the native
> decoder is present (on C3, `dts_audiodec` at rank 290) and brings its own loudness
> handling, so routing to it beats injecting ours. The app refuses the gain/DRC/A-B
> endpoints off `webos25-armel-gst124` on purpose — see
> [`MULTI-MODEL.md` §2.6](MULTI-MODEL.md) before widening that gate.

Once DTS and TrueHD decode at all, they play **noticeably quieter** than LG's
native AAC / AC-3 / Atmos on the same TV. Root cause (confirmed against the
code, not guessed): LG's closed native decoders bake in dialnorm/DRC loudness
management; upstream `dtsdec` and stock ffmpeg's `mlpdec` apply **none** —
before the webOS 25 patches below, DTS decode was a straight `libdca`
float-to-S32 conversion and `mlpdec.c` had no loudness stage at all. There is
no per-stream DTS DIALNORM parsing here (`libdca` doesn't cleanly expose it)
— the fix is a fixed, user-tunable **make-up gain**, not true dialnorm
normalization.

**Mechanism:** each decoder gets a make-up gain (dB) applied to its decoded PCM
in **float**, immediately **before** the existing float→S32 scale/clamp, so
the pre-existing clipping guard still protects the output:

- **DTS** (`gstdtsdec.c`) — a `makeup-gain-db` GObject property
  (`:837-841`), mirroring the existing `drc` property. The default is read
  once at decoder init (`:988-1000`) from the config file below; the gain is
  cached as a linear multiplier (`gst_dtsdec_apply_makeup_gain_db`, `:924-937`)
  and applied in the per-sample loop (`:1461-1479`, the `gain_only` path taken
  when DRC is off) before the S32 clamp. `dts2lpcm` inherits this for free —
  it wraps the same `dtsdec` internally.
- **TrueHD/MLP** — `build-truehd.sh` applies an inline source patch to ffmpeg
  n4.4.4's `libavcodec/mlpdec.c` before `./configure` (`:64-257`). The patch
  reads the config file once at `mlp_decode_init`, caches a linear multiplier
  on `MLPDecodeContext`, and scales the packed PCM output in `output_data()`
  with a saturating S32/S16 clamp. The patch is confined to the mlp/truehd
  translation unit — it cannot touch AAC/AC-3/E-AC-3/ALAC decoding in the
  shared `libgstlibav.so` (see `webos25/restore/src/TRUEHD-BUILD.md`).

**Config-file contract (identical for both codecs):**

| Codec  | Path | Read by |
|---|---|---|
| DTS    | `/var/lib/webosbrew/dts25/gain.conf`   | `gstdtsdec.c` decoder init |
| TrueHD/MLP | `/var/lib/webosbrew/truehd/gain.conf` | patched `mlpdec.c` decoder init |

A single ASCII float = make-up gain in **dB** (e.g. `6.0`). `#` comment lines
and blank lines are ignored; leading/trailing whitespace tolerated. Missing
file, empty file, or unparseable content → **0.0 dB = unity = today's
behavior** (never fails decode). Parsed value is clamped to **[-20.0,
+20.0] dB** before conversion to a linear multiplier
(`linear = pow(10, dB/20)`); 0.0 dB always yields an exact linear `1.0` (no
`pow`/`powf` rounding), so the unity case is a bit-exact no-op versus stock.
Takes effect on the **next playback** (decoder init reads the file fresh) —
no registry re-init needed.

**First-run seeding.** Gain and DRC were deliberately **opt-in** at first: with
no config file the decoders stay fully inert, so installing could not alter
anyone's sound while the DSP curve was still unproven. Now that the curve is
validated on-device, that default is the wrong one — an untouched install
leaves DTS/TrueHD quieter and un-managed next to native AAC/AC-3, which is
exactly what the feature exists to fix. So `restore/install.sh`
(`seed_gain_conf`) and the app's Enable (`w25GainConfSeedScript`) now write a
starting config of **+5.0 dB, `drc=line` 100/100, `center=0.0`** (preset
*Medium*) for both codecs — the value the maintainer settled on **by ear** on a
real C5 with this same preset active, not a theoretical figure — but
**only when the file does not already exist**,
so re-running install or Enable never overwrites saved settings. The inert
0.0 dB / DRC-off path above therefore applies only if the config is deleted
(e.g. by Uninstall) or was never seeded. Retune from the app or per
[`../restore/TUNING-RUNBOOK.md`](../restore/TUNING-RUNBOOK.md).

**App control:** the "DTS Enabler" app has a **Make-up gain & dynamic range**
card with a DTS group and a TrueHD group, each with a gain field (dB, range
[-20, +20], step 0.5), a DRC preset stepper, and a dialogue-boost field — see
[`../app/README.md#make-up-gain--drc-control`](../app/README.md#make-up-gain--drc-control).
No SSH or rebuild is needed to change any of it.

The same card has an **in-app A/B compare**: one press renders the bundled DTS
sample **twice** — once fully inert (DRC off, 0 dB gain/dialogue-boost) and
once with the saved settings — and reports the measured dB delta between
them, instead of relying on ear alone. Both variants are expressed as `dtsdec`
GObject properties on the render command (`drc-mode`, `drc-boost`, `drc-cut`,
`makeup-gain-db`, `center-boost-db`), so `gain.conf` is never written for the
comparison. Measurement is a second GStreamer `level`-element pass over each
render, parsed from `gst-launch-1.0 -m` — there is no ffmpeg on the TV, so
`level` is the only on-device measurement route available. Whether webOS will
actually play the rendered WAV back through an `<audio>` element is not
verified; if playback is refused the card falls back to numbers-only, and the
measured delta is still valid either way. Full mechanism, code pointers, and
the measured ground truth (mean/peak dB for `drc=off` vs `drc-mode=rf`) are in
[`../app/README.md#ab-compare-hear-the-drc-on-the-same-clip`](../app/README.md#ab-compare-hear-the-drc-on-the-same-clip).

### Dynamic range compression (DRC) + dialogue boost

The make-up gain above raises everything equally, so it cannot fix a
different problem: native AC-3/E-AC-3/Atmos on this TV sound "managed" —
dialogue rides above effects — while DTS/TrueHD do not. That is not a
guess; it is provable from LG's own kernel driver.

**The evidence.** LG's audio DSP driver (`kdriver/core/aud/common/imc/cmd/`,
from the webOS 25 kernel source drop — `~/Downloads/webOS25 WG_2.0_3.tar.gz`
→ `soc_GO/linux-and-kdriver_paluma-5-*.tar.gz`) exposes exactly the parameter
model reverse-engineered here:

- `module_cmd_ddc.h` — `DdcCmdSetParam.drc_mode`: **0 = Line (the default)**,
  1 = RF; `drc_cut_scl_factor` and `drc_boost_scl_factor`, both **0–100**.
  Line mode is Dolby's broadcast/Line profile — **DRC is ON by default** for
  Dolby streams on this TV.
- `module_cmd_dts.h` — `DtsCmdSetParam.drc_percent`, **0 (default)–100** →
  **DTS DRC is OFF by default**. LG does not compress DTS even though the
  parameter exists.
- LG's closed `libgstlgaudiodec.so` carries the matching strings
  `LGADEC_DRC_LINE` / `LGADEC_DRC_RF`, `LX_AUD_DECODER_DRC_{OFF,LINE,RF}`,
  `PROP_DOLBY_DRC_MODE`, `PROP_DEFAULT_PRL` — the same off/line/rf enum.
- On the decode side, ffmpeg's `mlpdec.c` (n4.4.4, stock) carries **zero**
  dialnorm/DRC/gain handling — TrueHD has no metadata this path could even
  use. `libdca` parses `dialog_norm` (`parse.c:251`) but never applies it,
  and its only level-adjustment hook, `DCA_ADJUST_LEVEL`, is downmix
  compensation (`downmix.c:98`), not dialnorm.
- TV settings observed: `autoVolume: off`, `clearVoice: off`,
  `soundMode: movie` — the user runs cinema mode and has declined LG's
  Clear Voice feature, so there is no native dialogue-lift path to lean on.

So native Dolby content is DRC'd by LG's DSP and DTS/TrueHD are not — the
dialogue-vs-effects gap is real and structural, not a mixing artifact. To
close it, both custom decoders now generate their own DRC, mirroring LG's
`mode` + `boost%` + `cut%` model. **This mirrors the shape LG documents its
defaults with — it is a documented-style approximation of Dolby's Line/RF
profiles, not Dolby's proprietary compression tables**, and every constant
in it is a tunable, commented value in the decoder source, not a licensed
figure.

**The curve.** Per-block (256 samples) RMS across the full-range channels
only (LFE excluded), converted to dBFS, mapped through a peak-and-decay
compression profile per mode:

| | null band | boost decays to 0 dB at | boost peaks at | cut ratio |
|---|---|---|---|---|
| `line` | −31…−20 dBFS | −85 dBFS | **+12 dB** @ −50.5 dBFS | 2:1, then 20:1 above −10 dBFS |
| `rf`   | −31…−24 dBFS | −85 dBFS | **+16 dB** @ −55.5 dBFS | 2:1, then 20:1 above −14 dBFS |

Below the peak the curve rises at 5:1 up to the cap; between the null band
and the peak it is a gentler 2:1. Unlike a classic compressor, the boost
region does not plateau at the cap all the way down to silence — it
**decays back to 0 dB at −85 dBFS**. That is deliberate: material below
roughly −60 dBFS is room tone and dither, not something meant to be
amplified 12–16 dB, and a held plateau meant a slow fade-out wound the gain
up to the cap so the next loud cue could swell and clip. There is also
**deliberately no silence gate** — an earlier design held the smoothed gain
during digital silence to prevent that same wind-up, but measurement showed
the gate made the worse case *worse* (a hard cut from an established high
gain froze at that gain, so the next cue peaked at −0.42 dBFS instead of
−12.01 dBFS with no gate at all). The boost decay does the wind-up-prevention
job instead, without that failure mode, so the gate was removed rather than
retuned.

The target gain is scaled by `drc_boost`/`drc_cut` percentages (mirroring
LG's scale factors) and smoothed one-pole in the dB domain (line: 10 ms
attack / 250 ms release; rf: 5 ms / 150 ms), then applied identically to
every channel — preserving the stereo/surround image — while a *separate*
centre-channel boost (`center`, dB) lifts dialogue specifically. See the
epic's DSP contract (`.orchestration/dts-truehd-drc/EPIC.md`) for the full
derivation and amendment history; the reference implementation is the
"DRC CORE" block in `gstdtsdec.c:196-735` (self-contained plain C, ported
byte-for-byte into the TrueHD `mlpdec.c` patch — see
[`../restore/src/TRUEHD-BUILD.md`](../restore/src/TRUEHD-BUILD.md)).

**Config format (extends the file above, backward compatible):** the bare
float on its own line still means make-up gain, unchanged. Optional
additional `key=value` lines:

| Key | Range | Default |
|---|---|---|
| `drc` | `off` \| `line` \| `rf` | `off` |
| `drc_boost` | `0..100` | `100` |
| `drc_cut` | `0..100` | `100` |
| `center` | `-10..+10` (dB) | `0` |

`#` comments and blank lines are ignored, unknown keys are ignored (forward
compatible). Out-of-range **finite** values clamp to the nearest bound
(e.g. `drc_boost=-50` → `0`, `center=-50` → `-10`); `±inf` clamps the same
way. A value that fails to parse **at all — including `nan`/`NaN`** — is
treated as unparseable, not as an out-of-range number (NaN has no
ordering, so "clamp" is meaningless for it), and that key falls back to its
own default instead. This matters concretely: clamping a NaN `center` to
its `lo` bound would silently turn a parse failure into a −10 dB dialogue
**cut**, and clamping a NaN `gain` to `lo` would silently attenuate by
20 dB — the opposite of what anyone editing this file intends. **Known,
accepted exception:** the GObject *property* setters (`makeup-gain-db`,
`center-boost-db`) do go through the ordinary finite clamp and land on
their `lo` bound for a NaN input — safe (still finite, no undefined
behaviour) and reachable only via `gst-launch`/the self-test, since the
app and the config-file reader both go through the NaN-aware path above.

**Presets** (what the app's DRC preset stepper writes):

| Preset | `drc` | `drc_boost` | `drc_cut` |
|---|---|---|---|
| Off    | `off`  | — | — |
| Light  | `line` | 50  | 50  |
| Medium | `line` | 100 | 100 |
| Night  | `rf`   | 100 | 100 |

**Inert by default:** `drc=off`, `center=0`, `gain=0` bypasses the whole
gain path — no multiply at all in the per-sample loop — so an un-tuned
install decodes bit-identical to the previous (make-up-gain-only) build.

**Tuning + release runbook:** see
[`../restore/TUNING-RUNBOOK.md`](../restore/TUNING-RUNBOOK.md) for the by-ear
tuning procedure and the rebuild → on-device-verify → recommit → tag release
loop required whenever `gstdtsdec.c` or `build-truehd.sh` changes (per
[`.claude/rules/releasing.md`](../../.claude/rules/releasing.md)).
