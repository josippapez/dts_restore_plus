# DTS + TrueHD/MLP audio restore for webOS 25 (LG C5)

Restores **DTS** *and* **Dolby TrueHD / MLP** audio playback on a rooted LG C5 /
webOS 25 TV. Both codecs are **verified working on a real LG C5**, persistent
across reboot (a boot hook re-applies everything). No stock LG library or config
file is modified in place — every change is a **bind-mount** over an original,
so uninstall is a clean revert.

## Quick install (prebuilt — no build needed)

The DTS and TrueHD decoders — plus the container demuxers — are **prebuilt and
bundled** in `restore/` (`restore/out/` + `restore/truehd-out/` +
`restore/demux-out/`), and `restore/install.sh` is a **single self-contained
script** (the boot hook is embedded in it). You do NOT need Docker or to build
anything.

On a rooted webOS-25 TV with the Homebrew Channel + root SSH:

```sh
# from your computer: copy the restore/ folder (or the release tarball) to the TV
scp -r webos25/restore root@<TV-IP>:/tmp/dtsrestore

# on the TV, as root:
cd /tmp/dtsrestore && sh install.sh
```

That one command stages both decoders, applies the routing overrides (all
bind-mounts), installs the reboot-persistent boot hook, and activates it now —
then play a DTS or TrueHD file. To revert: `sh uninstall.sh`.

## Install the app via the Homebrew Channel (no SSH)

Prefer a GUI? Add this repository in the Homebrew Channel
(**Settings → Add repository**):

```
https://josippapez.github.io/dts_restore_plus/api/apps.json
```

Then install **DTS Enabler** from the list (Enable / Disable / Uninstall + a
self-test and play-by-ear). The `.ipk` is pulled from the GitHub release and
sha256-verified; updates flow automatically. Requires a rooted TV with the
Homebrew Channel. (The CLI `restore/install.sh` above remains the SSH-based route.)

## Folder layout

- `restore/` — the CLI tool: prebuilt decoders (`out/`, `truehd-out/`) + container
  demuxers (`demux-out/`) + `install.sh`/`uninstall.sh` + the `build*.sh` scripts to
  rebuild them (Docker).
- `app/` — the "DTS Enabler" webOS homebrew app (GUI enable/disable/uninstall).
- `docs/` — design notes (`MULTI-MODEL.md`), the target-detection probe
  (`detect-target.sh`), background (`WEBOS25-DTS.md`), and `experimental/`.

*(To rebuild the binaries instead of using the bundled ones, see
`restore/build.sh` (DTS) and `restore/build-truehd.sh` (TrueHD) — requires Docker.)*

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

2c. **Container demuxers (mp4/ts/m2ts DTS)** — patched `libgstisomp4.so` and
   `libgstmpegtsdemux.so` (built with `dca=true` **and** `dts_support` defaulting
   TRUE) are staged in `/var/lib/webosbrew/demux25/` and **bind-mounted over** LG's
   `/usr/lib/gstreamer-1.0/libgst{isomp4,mpegtsdemux}.so` **before** the registry
   regen, so the demuxers emit `audio/x-dts` for mp4/ts/m2ts instead of an
   untargetable fourcc. Video pads (H.264/HEVC/DV) are untouched.

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

**Container support:** **MKV, `.mp4`, and `.ts`/`.m2ts` are all supported** for DTS. LG ships
`qtdemux`/`tsdemux` with DTS demuxing compiled out *and* gated behind a runtime `dts_support`
property that defaults FALSE — so stock mp4 DTS came out as untargetable `audio/x-gst-fourcc-dtsc`
and `.ts` DTS didn't route. The fix rebuilds those two demuxers from LG's webOS-25 source with
`dca=true` **and** a 2-line patch flipping `dts_support` to default TRUE
(`qtdemux.c` / `tsdemux.c`), staged in `restore/demux-out/` and bind-mounted by the boot hook.
Verified on the C5 against **real Blu-ray DTS-HD MA content**: a 5.1 `.ts` sample decodes to
`audio/x-raw, S32LE, 6 channels (FL FR FC LFE RL RR), 48000 Hz`, an `.mp4` (dtsc) decodes to PCM,
and normal AAC mp4 playback is unaffected. (TrueHD is verified in MKV; `.mp4`/`.ts` TrueHD is not
separately tested.)

**Caveats (honest):**
- **Discrete 5.1 reaches LG's sink — confirmed in real playback, no downmix in the pipeline.**
  Measured on a real C5: `dtsdec` emits native discrete 5.1 (6 channels of distinct content) as
  S32LE/48 kHz, matching a reference DTS core decoder within ~0.1–0.2 dB per channel. During actual
  Media-Player playback the GStreamer debug log shows LG's `audiosink` negotiating
  `audio/x-raw, S32LE, 48000, channels=6` (its sink pad advertises `channels=[1,10]`), so full 5.1
  PCM is delivered end-to-end to LG's audio HAL — there is **no stereo downmix anywhere in the
  GStreamer path** (unlike the CX/upstream tool, which force-downmixes to 2.0). A BD-LPCM re-frame is
  therefore **not needed** to reach a multichannel sink. **The only remaining variable is the TV's
  own output stage:** internal speakers fold 5.1 into the built-in array, while **HDMI eARC/optical to
  an AVR** carries the multichannel PCM subject to the "Digital Sound Output" setting. Confirm 5.1 on
  an AVR's input display.
- **DTS-HD:** `avdec_dca` decodes the DTS **core**, not the DTS-HD MA lossless (XLL) extension.
  **TrueHD:** decoded as base channels (Atmos objects fold in).
- **No bitstream passthrough** to an AVR (decode-to-PCM only) — out of scope.

## Build

Both builds are reproducible Docker / cross-builds and print an ABI report
(ELF class, `e_flags`, `NEEDED`/`RPATH`, max GLIBC symbol) so you can confirm
soft-float `0x05000200` before deploying.

```sh
./build.sh          # -> out/libgstdtsdec.so, out/libdca.so.0     (patched dtsdec)
./build-truehd.sh   # -> truehd-out/libgstlibav.so + libav*/libsw* (gst-libav + ffmpeg n4.4.4)
./build-demux.sh    # -> demux-out/libgst{isomp4,mpegtsdemux}.so   (DTS demux, dts_support=TRUE)
```

`build.sh` needs Docker with arm64 emulation
(`docker run --privileged --rm tonistiigi/binfmt --install arm64` once).
`build-truehd.sh` runs inside `debian:11-slim --platform linux/arm64`. See
`src/gstdtsdec.c` (DTS patch) and `src/TRUEHD-BUILD.md` (TrueHD recipe notes).

The built `.so` artifacts are committed under `restore/out/` and
`restore/truehd-out/` so `install.sh` can deploy without a rebuild.

## Install (on the TV, as root)

Copy the `restore/` folder (with populated `out/` and `truehd-out/`) to
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
