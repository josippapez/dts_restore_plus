# DTS + TrueHD/MLP audio restore for webOS 25 (LG C5)

Restores **DTS** *and* **Dolby TrueHD / MLP** audio playback on a rooted LG C5 /
webOS 25 TV. Both codecs are **verified working on a real LG C5**, persistent
across reboot (a boot hook re-applies everything). No stock LG library or config
file is modified in place — every change is a **bind-mount** over an original,
so uninstall is a clean revert.

## Quick install (prebuilt — no build needed)

The DTS and TrueHD decoders are **prebuilt and bundled** here (`out/` + `truehd-out/`),
and `install.sh` is a **single self-contained script** (the boot hook is embedded
in it). You do NOT need Docker or to build anything.

On a rooted webOS-25 TV with the Homebrew Channel + root SSH:

```sh
# from your computer: copy this folder (or the release tarball) to the TV
scp -r webos25 root@<TV-IP>:/tmp/

# on the TV, as root:
cd /tmp/webos25 && sh install.sh
```

That one command stages both decoders, applies the routing overrides (all
bind-mounts), installs the reboot-persistent boot hook, and activates it now —
then play a DTS or TrueHD file. To revert: `sh uninstall.sh`.

*(To rebuild the binaries yourself instead of using the bundled ones, see
`build.sh` (DTS) and `build-truehd.sh` (TrueHD) — requires Docker.)*

## Root cause (verified on-device)

LG ships webOS 25 with **no DTS decoder and no TrueHD decoder**, and:

- **DTS:** `matroskademux` re-tags the MKV DTS track as
  `audio/x-unknown, codec-id=(string)A_DTS` (raw DTS bytes preserved). There is
  no `dts_audiodec` / `avdec_dca` to decode it.
- **TrueHD:** LG's `libgstlibav.so` is built **without** the TrueHD/MLP
  decoders, and its HW audio path (`audiooutputd`) does not handle TrueHD.

**The crux — integer PCM only:** LG's `audiosink` accepts only integer PCM
(S8..S32), **no float**. A decoder that emits `F32LE` is negotiated and then
**silently dropped** (no audio, no error). Both fixes therefore produce/keep
**S32LE**:

- `dtsdec` is patched to convert libdca's float output to **S32LE** (clamped).
- `avdec_truehd` already emits native **S32** PCM, so it works as-is.

## Target ABI (the other crux)

- LG C5, OLED77C51LA, chassis o22n3, webOS 10.3.1 "Rockhopper".
- Kernel is aarch64, but the **GStreamer userspace is 32-bit ARM, EABI5
  soft-float** (`ld-linux.so.3`, `e_flags 0x05000200`) — Debian's `armel` port.
- glibc **2.35**, GStreamer **1.24.0**, glib 2.72.
- All shipped `.so` are armel soft-float with max GLIBC symbol `<= 2.35`.
  (CX/dts_restore's armv7 hard-float GStreamer 1.14 binaries are incompatible.)

## How the fix works

Everything below is applied at boot by the canonical `init_dts25.sh` (installed
verbatim and symlinked from `/var/lib/webosbrew/init.d/restore_dts25`):

1. **DTS decoder** — the patched `dtsdec` (sink caps widened to also accept
   `audio/x-unknown, codec-id=A_DTS`; output S32LE) + bundled `libdca.so.0` are
   staged in `/var/lib/webosbrew/dts25/{,libs/}`. `decodebin`/`decproxy` autoplug
   it directly onto LG's retagged stream.

2. **TrueHD decoder** — our `libgstlibav.so` (with `avdec_truehd`/`avdec_mlp`)
   + minimal ffmpeg libs are staged in `/var/lib/webosbrew/truehd/{,libs/}`, and
   our libgstlibav is **bind-mounted over** LG's TrueHD-less
   `/usr/lib/gstreamer-1.0/libgstlibav.so` (name-dedup would otherwise pick LG's).

3. **Codec capability** — `TRUEHD` + `MLP` audio-codec objects are added to
   `/etc/umediaserver/device_codec_capability_config.json` so `umediaserver`
   allocates a decoder resource for those codecs. Applied by bind-mounting an
   **edited copy** over the original.

4. **The rank lever (key for TrueHD)** — `avdec_truehd=310` and `avdec_mlp=310`
   are added to the `[sw_decoder]` section of `/etc/gst/gstcool.conf`, so LG
   autoplugs the **SW** decoder instead of its HW path. Applied by bind-mounting
   an edited copy.

5. **Registry** — the media GStreamer registry is regenerated (with
   `LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs` and a plugin path that
   includes `/var/lib/webosbrew/dts25`) so it contains both `dtsdec` and
   `avdec_truehd`, then written to `/mnt/flash/data/gst_1_0_registry.arm.bin`.
   The registry is only overwritten if the regen actually contains the decoders.

**Config overrides are generated on the TV at install time** by editing the TV's
own live `/etc` files (see below) — this package **ships no LG config file**.

## Per-codec status

| Codec        | Element        | Output | Status on LG C5                 |
|--------------|----------------|--------|---------------------------------|
| DTS / DTS-HD | `dtsdec` (patched) | S32LE 5.1 | **Verified, persistent** |
| TrueHD       | `avdec_truehd` | S32LE (up to 7.1) | **Verified, persistent** |
| MLP          | `avdec_mlp`    | S32LE  | Enabled alongside TrueHD        |

## Build

Both builds are reproducible Docker / cross-builds and print an ABI report
(ELF class, `e_flags`, `NEEDED`/`RPATH`, max GLIBC symbol) so you can confirm
soft-float `0x05000200` before deploying.

```sh
./build.sh          # -> out/libgstdtsdec.so, out/libdca.so.0     (patched dtsdec)
./build-truehd.sh   # -> truehd-out/libgstlibav.so + libav*/libsw* (gst-libav + ffmpeg n4.4.4)
```

`build.sh` needs Docker with arm64 emulation
(`docker run --privileged --rm tonistiigi/binfmt --install arm64` once).
`build-truehd.sh` runs inside `debian:11-slim --platform linux/arm64`. See
`src/gstdtsdec.c` (DTS patch) and `src/TRUEHD-BUILD.md` (TrueHD recipe notes).

The built `.so` artifacts are checked into `out/` and `truehd-out/` (git-ignored)
so `install.sh` can deploy without a rebuild.

## Install (on the TV, as root)

Copy this whole `webos25/` folder (with populated `out/` and `truehd-out/`) to
the TV, then:

```sh
sh install.sh
```

`install.sh` stages both payloads, **generates both config overrides by editing
the TV's live /etc files**:

- capability config: `awk` inserts the `TRUEHD` + `MLP` objects **after the DTSE
  entry** of `/etc/umediaserver/device_codec_capability_config.json`;
- gstcool: `awk` inserts `avdec_truehd=310` + `avdec_mlp=310` **right after the
  `[sw_decoder]` header** of `/etc/gst/gstcool.conf`;

writes the edited copies under `/var/lib/webosbrew/truehd/`, installs the
canonical `init_dts25.sh`, symlinks the boot hook, applies everything now, and
restarts `starfish-media-pipeline`. It is idempotent, guarded, logs to
`/tmp/dts25.log`, and always exits 0 (safe as a boot hook).

Remove everything with:

```sh
sh uninstall.sh     # unmounts all four binds, removes the state dirs + hook
```

A reboot after uninstall guarantees a fully clean state.
