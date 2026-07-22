# Restoring DTS on webOS 25 (C5 / G5 and other GStreamer 1.24 sets)

**The `dts_restore` binaries in this repo do NOT work on webOS 25.** They are
GStreamer **1.14.4 / armv7 (OLED CX)** libraries; webOS 25 TVs (e.g. LG C5,
chassis `o22n3`) run **GStreamer 1.24 on aarch64**. The `.so` files are
incompatible on two counts (CPU architecture *and* GStreamer version) and will
not load — bind-mounting them **breaks** MKV/MP4 playback instead of adding DTS.

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
