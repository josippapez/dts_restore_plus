# DTS restore for webOS 25 (LG C5)

Restores DTS audio playback on a rooted LG C5 / webOS 25 TV by cross-building a
patched GStreamer `dtsdec` decoder and injecting it into the media pipeline's
plugin registry — without modifying any stock LG library.

## What it is

LG ships **no DTS decoder** on webOS 25 and `matroskademux` re-tags the MKV DTS
track as `audio/x-unknown, codec-id=(string)A_DTS` (raw DTS bytes preserved).
This package supplies the missing decoder:

- `src/gstdtsdec.c`, `src/gstdtsdec.h` — the `dtsdec` plugin, vendored from
  gst-plugins-bad 1.22.0. The **only** functional change vs. upstream is the
  sink pad caps, widened to also accept LG's `audio/x-unknown, codec-id=A_DTS`
  so `decodebin`/`decproxy` autoplug it directly onto LG's retagged stream.
- `build.sh` — Docker cross-build producing the armel plugin + bundled libdca.
- `install.sh` / `uninstall.sh` — on-TV deploy / removal (registry injection).

## Verified target ABI (on-device)

- LG C5, OLED77C51LA, chassis o22n3, webOS 10.3.1 "Rockhopper".
- Kernel is aarch64, but the **GStreamer userspace is 32-bit ARM, EABI5
  soft-float** (`ld-linux.so.3`, e_flags `0x05000200`) — Debian's `armel` port.
- glibc **2.35**, GStreamer **1.24.0**, glib 2.72.
- The plugin is built against gst-plugins-bad 1.22 headers; that ABI is stable
  within 1.2x and loads fine in the TV's 1.24 runtime.
- CX/dts_restore binaries are armv7 **hard-float** GStreamer 1.14 — incompatible
  on two counts (float ABI + GStreamer version). This package targets soft-float.

## Root cause (verified)

DTS is disabled by (a) shipping no `dts_audiodec`/`avdec_dca`, and (b)
`matroskademux` mapping the DTS track to `audio/x-unknown, codec-id=A_DTS`.
No runtime property re-enables the mapping. The fix is a decoder whose sink caps
match that exact media type, plus getting it into the media registry.

## How the fix works

1. **Patched caps** — `dtsdec` advertises
   `audio/x-dts; audio/x-private1-dts; audio/x-unknown, codec-id=(string)A_DTS`,
   so autoplugging picks it up on LG's retagged stream. Decode body is unchanged:
   it parses the raw DTS elementary stream via `libdca` and emits `audio/x-raw`.
2. **Registry injection** — regenerate the media GStreamer registry (including
   `dtsdec`) into `/tmp` and **bind-mount** it over the file the media pipeline
   reads (`/mnt/flash/data/gst_1_0_registry.arm.bin`). A boot hook re-applies it.

## Build

Requires Docker with arm64 emulation (`docker run --privileged --rm
tonistiigi/binfmt --install arm64` once).

```sh
./build.sh
```

Outputs to `out/`:

- `libgstdtsdec.so` — patched decoder (armel soft-float, max GLIBC ≤ 2.35).
- `libdca.so.0` — DTS decode library (armel), bundled for the TV.

`build.sh` prints the ELF class, `e_flags`, `NEEDED`/`RPATH`, and max GLIBC
symbol so you can confirm the ABI before deploying.

## Install (on the TV, as root)

Copy this `webos25/` folder (with a populated `out/`) to the TV, then:

```sh
sh install.sh
```

This installs `libgstdtsdec.so` to `/var/lib/webosbrew/dts25/`, `libdca.so.0`
to `/var/lib/webosbrew/dts25/libs/`, writes `init_dts25.sh`, symlinks the boot
hook `/var/lib/webosbrew/init.d/restore_dts25`, applies it immediately, and
restarts `starfish-media-pipeline`. It is idempotent and logs to `/tmp/dts25.log`.

Remove everything with:

```sh
sh uninstall.sh
```

## STATUS — honest

**Proven (on hardware / in gst-launch):**

- The patched `dtsdec` loads in the TV's GStreamer 1.24 (`gst-inspect-1.0` OK).
- It decodes a DTS elementary stream to `audio/x-raw F32LE`.
- `decodebin` **autoplugs the patched `dtsdec` for LG's
  `audio/x-unknown / A_DTS`** stream and outputs `audio/x-raw F32LE 5.1` — the
  key routing proof.
- `dtsdec` appears in the media registry (rank primary) at the bind-mounted path.

**Open item being verified (needs the TV, serial):**

- Full playback through LG's `starfish-media-pipeline` + `decproxy`. Two unknowns:
  does `decproxy` autoplug `dtsdec` the same way `decodebin` does, and does LG's
  audio sink accept `dtsdec`'s **F32LE (possibly multichannel)** output, or must
  the output be forced to stereo / S16? `decproxy` cannot be exercised
  standalone in `gst-launch`, so this is confirmed on-device.

If the sink rejects F32LE/5.1, the follow-up is to constrain `dtsdec`'s src caps
(force S16 and/or stereo downmix) — a source change + rebuild, no redeploy of the
injection mechanism.
