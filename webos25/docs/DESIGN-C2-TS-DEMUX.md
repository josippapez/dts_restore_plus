# DESIGN — TS/M2TS containers for the C2/G2/CS profile

Status: **DRAFT / NOT IMPLEMENTED**. Scoped 2026-08-24.

## The blocker is the container, not the channel count

The app bundles three DTS samples. Measured with `ffprobe`:

| file | codec | profile | channels | plays on C2 |
|---|---|---|---|---|
| `DTS-in-mp4.mp4` | dts | DTS-HD MA | 6 — `5.1(side)` | **yes** |
| `DTS-HD-MA-5.1.ts` | dts | DTS-HD MA | 6 — `5.1(side)` | no |
| `DTS-HD-MA-5.1.m2ts` | dts | DTS-HD MA | 6 — `5.1(side)` | no |

All three are 6-channel DTS-HD MA, so "these files are multichannel" does **not**
explain the failures: a 5.1 file already plays in MP4 on that TV today (arriving as
2.0, because the payload decoder downmixes — see
[`DESIGN-C2-MULTICHANNEL.md`](DESIGN-C2-MULTICHANNEL.md)).

The only difference is the container. `payload/cx/` ships `libgstlibav.so`,
`libgstisomp4.so`, `libgstmatroska.so` and `libgstisomp4_1_8.so` and **no TS
demuxer** — `MULTI-MODEL.md` §2.3 states "No legacy `libgstmpegtsdemux.so` is
packaged", and §2.1 lists it as the one CX binary never built.

## LG's 1.14.4 TS demuxer needs no patch

The webOS-25 (1.24) work needs a source patch because LG wrapped upstream's DTS
handling in `#ifdef DTS_SUPPORT` plus a runtime `demux->dts_support`, defaulting
FALSE (`tsdemux.c:1179`), and `build-demux.sh` flips that default.

**That gate does not exist in LG's 1.14.4 tree.** In
`lgstreamer/gst-plugins-bad@lg`, `gst/mpegtsdemux/tsdemux.c` contains zero
occurrences of `DTS_SUPPORT`, its sink template advertises
`audio/x-dts; audio/x-dtsh; audio/x-dtse; audio/x-dtsl` (line 531), and
`case ST_PS_AUDIO_DTS:` sets `audio/x-dts` unconditionally (line 3414). The
`DRF_ID_DTS1/2/3/DTSH` registration paths and `set_caps_for_private_dts()` are
present too. LG added the gate somewhere after 1.14.4.

So this is a **plain build of published source**, with no patch step at all.

## Why a 1.14 plugin loads on a 1.18 TV

`gst_plugin_check_version()` rejects a plugin only when
`major != GST_VERSION_MAJOR || minor > GST_VERSION_MINOR` (`gstplugin.c:487`).
14 ≤ 18, so it loads — which is already demonstrated: the four 1.14.4 binaries in
`payload/cx/` are running on that owner's 1.18.5 TV right now.

Keeping LG's own tree (rather than upstream 1.18) also keeps LG's TS extensions —
`app_type`/RTC, dtcpip, miracast LPCM and the LG descriptor paths — which an
upstream demuxer would drop, with unknown effect on live TV and DVR.

## Build

Same toolchain as the existing CX payload, targeting armel soft-float /
`ld-linux.so.3` / ELF32 ARM EABI5:

- source: `lgstreamer/gst-plugins-bad` branch `lg` (1.14.4), `gst/mpegtsdemux` only;
- no patch;
- output to a new directory, leaving the committed 1.24 `demux-out/` untouched.

## Deployment

Per-target payload, matching the multichannel plan:

- add `libgstmpegtsdemux.so` to a C2-specific payload directory;
- bind it over `/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so` only for
  `webos22-o22-gst118`;
- extend `c2_payload` from four files to five, and `c2_apply`/`c2_detach`
  accordingly — the existing ownership, drift and recovery checks then cover it
  with no new mechanism;
- update the profile's advertised capability: it currently reports "MP4 active
  (TS/M2TS unavailable)" (`app/js/app.js`) and the self-test is MP4-only, both of
  which would become wrong.

## Unverified — do not state these as facts

1. **Does a 1.14.4 TS demuxer actually work against a 1.18.5 core at runtime?**
   It will *load* (ABI rule above). Whether tsdemux's interaction with the 1.18
   `mpegtsbase`/`mpegtspacketizer` in the same plugin is sound is untested — note
   the whole `mpegtsdemux` plugin is one `.so`, so `mpegtsbase` comes along with it
   and the pairing is internally consistent.
2. **Live TV and DVR regression.** The bind is system-wide, so broadcast playback
   must be checked before this ships, not only file playback.
3. **HDMV/BD paths.** `WEBOS25-DTS.md` records that which DTS recognition site a
   `.ts` lands in is decided entirely by its PMT; the 1.14 matrix has not been
   compared against the 1.24 one used for the C5 samples.

## Order of work

1. Build the 1.14.4 demuxer and confirm it loads on a C2 (`gst-inspect-1.0 tsdemux`).
2. Play the two bundled samples; they are known-good 5.1 DTS-HD MA.
3. Check live TV and a DVR recording still play before shipping.
4. Only then extend the payload, the self-test, and the capability wording.
