# DESIGN — C3/G3/M3 profile (`webos23-o22n-gst118`)

Status: **DRAFT / NOT IMPLEMENTED.** No C3 hardware has been available to this
project, and the project's rule is that a bind mechanism is verified on a real TV
before it ships. This document is the spec to implement against when one appears.

Evidence base: extracted C3 webOS-23 firmware (see
[`FIRMWARE-COMPATIBILITY.md`](FIRMWARE-COMPATIBILITY.md)), LG's own webOS GStreamer
source, upstream `lgstreamer/dts_restore`'s implementation, and one owner report.

## Why C3 needs a profile at all

C3 is the generation that is easiest to get wrong, in both directions.

LG **restored** DTS for the 2023 line, and the artifacts agree: `libgstlgaudiodec.so`
registers `dts_audiodec`, and `gstcool.conf` *raises* it from 128 to **rank 290**. So
"C3 has no DTS decoder" is false, and refusing C3 (as this app originally did) is
wrong.

But "C3 works, nothing needed" is also false. The stock demuxers are nerfed: Matroska
emits `audio/x-unknown, codec-id=A_DTS` rather than `audio/x-dts`, so nothing ever
hands the 290-ranked decoder a stream it will accept. **DTS is present and
unreachable.** Owner-reported (2026-08-21): upstream `dts_restore` had to be installed
on a real C3 to get DTS playing, despite the native decoder.

## The mechanism: demuxers only — and no rank change

This is the part that differs from every other profile here, and it is the whole point.

Upstream's `init_dts.sh` does three things: bind un-nerfed demuxers, regenerate the
registry, and `sed "s/avdec_dca=0/avdec_dca=290/" /etc/gst/gstcool.conf`. That third
step is correct **for CX**, which has no native DTS decoder — it promotes ffmpeg's
`avdec_dca` so something can decode.

On C3 that step is unnecessary and arguably harmful. `dts_audiodec` is already at 290,
so raising `avdec_dca` to 290 creates a tie between LG's hardware-backed decoder and a
software one, resolved by registry order rather than intent. Routing to LG's decoder is
strictly better:

- LG's decoder brings its own dialnorm/DRC loudness handling, so the make-up-gain/DRC
  machinery this project needs on webOS 25 is **not needed here at all** (see
  [`MULTI-MODEL.md` §2.6](MULTI-MODEL.md))
- DSP-offloaded decode rather than CPU software decode
- Potentially DTS-HD MA / DTS:X rather than the 5.1 core `libdca` gives us
- No decoder payload at all, so no ABI, soft-float, GStreamer-version or
  `DT_NEEDED` matching to get wrong

So: **bind demuxers, regenerate the registry, touch no ranks and no `gstcool.conf`.**

### Payload

Four files, all already tracked in the repo root `gst/` and already packaged into
`payload/cx/` by `release.yml`. **No new payload is introduced.**

| file | why |
|---|---|
| `libgstmatroska.so` | the confirmed blocker — MKV is where the gate bites |
| `libgstisomp4.so` | MP4 DTS (`audio/x-gst-fourcc-dtsc` stock) |
| `libgstisomp4_1_8.so` | the `_1_8` variant, bound only when the stock target exists |
| `libgstmpegtsdemux.so` | `.ts`/`.m2ts` |

Deliberately **excluded: `libgstlibav.so`.** Binding it would inject ffmpeg's decoders
and reintroduce the routing ambiguity this design exists to avoid. Its absence is what
keeps decode on LG's native path.

These are LG's GStreamer **1.14.4** builds being loaded by a **1.18.5** runtime. That
is not assumed safe — it is what upstream shipped and what the owner report confirms
worked on a real C3. It must still be gated by a loader check (below), and it is the
single biggest reason this profile cannot ship unverified.

## Gate

Mirror the C2 profile's exact-match posture; do not invent a looser one.

- OTA `HE_DTV_W23O_AFABATAA`, product `OLED*C3*`/`OLED*G3*`/`OLED*M3*`, known board
  `o22n`
- webOS release / platform 8.3.1, firmware `03.33.25.01`
- GStreamer `1.18.5`, `/lib/ld-linux.so.3`, soft-float
- Stock SHA-256s from the extracted firmware: `libgstlgaudiodec.so`
  `698d578f00a50164770c30f5d1058f90c5824bf720b87ea6b9486ae8e00e90e9`, `libgstlibav.so`
  `99156a09fec83e869533f3e1359732fed8f83b4f70dcca44871fd5facadea2df`,
  `libgstmatroska.so` `d07223a1cf35739690df9cc35f1a3303e3251b303085550816fbe67e8e879f6c`
- `LD_TRACE_LOADED_OBJECTS` must resolve for each bound `.so` before anything mounts
- **Additionally, and specific to this profile:** `dts_audiodec` must be registered.
  If it is not, this is not the C3 case the design was written for — refuse. That is
  the behavioural check that makes the profile self-limiting.

GStreamer 1.18.5 alone must never select it, exactly as 1.18.2 alone must never select
C2. Two-step experimental opt-in, and it reports **hardware verification NO** until
someone runs it.

### An upgraded C3 does NOT come here

A C3 upgraded to webOS 25 (firmware `33.31.x`, platform 10.3.1 — confirmed from the
EPK header of `…o22n-papikonda-3006-33.31.68…`) reports GStreamer 1.24 and is handled
by the existing `webos25-armel-gst124` path, where it will hash-mismatch the C5 row and
land on `unverified` + opt-in. That is correct and needs no change. This profile is
only for a C3 still on webOS 23.

## State, ownership, teardown

Reuse the C2 engine's shape verbatim rather than writing a third one — dedicated
namespace `/var/lib/webosbrew/dtsenabler/c3`, hook
`/var/lib/webosbrew/init.d/restore_dts_c3`, complete-baseline requirement, recovery
marker, mount-source ownership checks, allowlist-before-mutation, snapshot-and-restore
on any cleanup failure, no recursive removal. First enable refuses if a legacy CX hook,
a C2 install, or any managed bind is already live, so three mechanisms can never
co-own a TV.

Simpler than C2 in two ways: no `gstcool.conf` override to generate or bind, and no
decoder payload to trace.

## What must be verified before this ships

1. Does binding the four 1.14 demuxers over 1.18.5 actually load? (loader gate should
   catch failure, but "loads" ≠ "works")
2. With ranks untouched, does `dts_audiodec` win — or does the stock `avdec_dca=0`
   leave nothing to tie against and route correctly by default?
3. Does MKV DTS actually play, and at how many channels?
4. Does LG's decoder apply its loudness handling, i.e. is the result *not* quiet the
   way our software path is on webOS 25?
5. Reboot, Disable, Uninstall, and the drift/recovery paths.
6. Does `.mp4`/`.ts` DTS also work, or only MKV?

Until (1)–(3) are answered on hardware this stays a draft. Do not ship it on the
strength of the owner report alone: that report covers *upstream's* mechanism, which
also raised `avdec_dca` — this design deliberately does not, so it is not the same
change and inherits none of its evidence.

## TODO

- [ ] Find a C3/G3/M3 owner willing to test, ideally still on webOS 23
- [ ] Confirm (1)–(3) above, then implement against the C2 engine
- [ ] If the demuxer-only routing turns out ambiguous in practice, fall back to
      upstream's `avdec_dca=290` and document the loudness consequence — our DRC does
      not apply off webOS 25, so a software-decoded C3 would be quiet with no remedy
- [ ] Recheck whether an upgraded C3 (webOS 25) keeps DTS; if LG stripped it there,
      those sets become ordinary `webos25-armel-gst124` candidates and this profile
      matters only for sets that never upgraded
