# dts_restore

Restore **DTS audio playback** (and add **Matroska Dolby Vision** support) on rooted LG webOS
TVs, where LG deliberately removed DTS decoding in firmware.

It works by bind-mounting recompiled GStreamer libraries — built from **LG's own released
sources**, with DTS demux/decode re-enabled — over the "nerfed" ones, and raising the DTS
decoder's priority. Nothing in the original firmware is modified; everything is applied as
temporary overlays that a full power-off reverts.

> Background and the full development history are in
> [RootMyTV issue #72](https://github.com/RootMyTV/RootMyTV.github.io/issues/72).

---

## This fork vs. upstream [`lgstreamer/dts_restore`](https://github.com/lgstreamer/dts_restore)

This is a fork of the original `dts_restore`. For **OLED CX and other webOS 3–6 TVs**, the
upstream project is the reference. This fork adds and changes the following on top of it:

**Added — webOS 25 support (new platform, `webos25/`):** the upstream tool is CX-only
(armv7 / GStreamer 1.14) and does **not** work on 2025 webOS-25 TVs (e.g. LG C5: 32-bit ARM
**soft-float**, GStreamer 1.24). `webos25/` is a self-contained tool that restores **both DTS
and TrueHD/MLP** there:
- `webos25/restore/` — prebuilt soft-float decoders (patched `dtsdec` → S32LE; `avdec_truehd` from
  a minimal ffmpeg) + patched container demuxers (`isomp4`/`mpegtsdemux` with `dts_support` default
  TRUE, so **DTS in `.mp4`/`.ts`/`.m2ts`** works, not just MKV) + a single self-contained
  `install.sh`. Verified playing on a real C5, including against real Blu-ray DTS-HD MA samples.
- `webos25/app/` — a "DTS Enabler" webOS homebrew app (GUI); `webos25/docs/` — design notes + the
  target-detection probe.

**Modified — CX tool hardening (root files, on top of upstream):**
- `install.sh` / `uninstall.sh` — fixed the `#!/usr/bin/env sh` vs bash shebang, the off-by-one
  media-player check (`[s]tarfish` never self-matches), non-idempotent `ln -s`, missing root/tool
  preflight; added a community model allowlist, `set -u`, a self-owned payload dir (no dangling
  symlink), and a complete uninstall (unmounts + payload removal).
- `init_dts.sh` (shipped instead of heredoc-generated) + externalized `downmix.conf`.
- This `README.md` (was `README.txt`): model table, persistence model, troubleshooting, changelog.

The upstream CX binaries in `gst/` are unchanged.

---

## Requirements

- A **rooted** LG TV (see [webosbrew.org/rooting](https://www.webosbrew.org/rooting/)) with the
  **Homebrew Channel** installed.
- **Root SSH access** to the TV (not telnet — telnet lacks the environment variables the
  installer needs).

## Install

```sh
cd /home/root
wget https://github.com/lgstreamer/dts_restore/archive/refs/tags/2.0.tar.gz
tar -xzvf 2.0.tar.gz
cd dts_restore-2.0
./install.sh          # add -y to skip the off-target prompt on unlisted models
```

The installer copies its payload to `/var/lib/webosbrew/dts_restore/` and registers a boot hook
at `/var/lib/webosbrew/init.d/restore_dts`, so the overrides **re-apply automatically on every
boot** until you uninstall. It also applies them immediately, so no reboot is needed the first
time.

## Uninstall

```sh
cd /home/root/dts_restore-2.0
./uninstall.sh
```

Then **fully power off** the TV to clear the GStreamer registry overlay. If **Quick Start+** is
enabled the TV never truly powers off — unplug it, or turn off *Settings → General → Quick Start+*
and power-cycle.

---

## Supported models

The shipped binaries are **GStreamer 1.14.4 / OLED CX** builds. They are field-confirmed working
on other 1.14-class LG sets (per issue #72). There is a single library set — models outside the
list below can still install (the installer just warns first), using the same binaries.

| Model family | webOS | GStreamer | Status |
|---|---|---|---|
| **OLED CX** | 5.x | 1.14.4 | **Reference target** |
| OLED BX | 5.x | 1.14-class | Same generation as CX |
| OLED C1 / G1 | 6.x | 1.14-class | Community-confirmed |
| OLED C2 / G2 | 6.x / "22" | 1.14-class | Community-confirmed |
| NanoCell / LCD (UN7xxx, NANO7xx, 2020–2022) | 5.x / 6.x | 1.14-class | Community-confirmed |
| webOS 22 / 23 / 24 | — | — | Not covered (no source drop; no durable root path) |
| webOS 25 (C5/G5) | "10" | **1.24** | Not these binaries — needs an aarch64/1.24 decoder ([WEBOS25-DTS.md](WEBOS25-DTS.md)) |

## Persistence — what survives a reboot

| Applied | Survives reboot? | Reverted by |
|---|---|---|
| `/var/lib/webosbrew/init.d/restore_dts` boot hook | **Yes** | `uninstall.sh` |
| Library bind-mounts over `/usr/lib/gstreamer-1.0/` | No (re-applied each boot by the hook) | reboot / uninstall |
| `/tmp/gstcool.conf`, registry overlay, `/tmp/dv_disable` | No (`/tmp` is volatile) | reboot |
| GStreamer registry partition | Semi (regenerated to `/tmp` and overlaid) | full power-off |

---

## Tuning the stereo downmix

DTS is decoded and **downmixed to 2.0 PCM** (see [Limitations](#limitations)). The mix is
controlled by `/var/lib/webosbrew/dts_restore/downmix.conf`:

```ini
front=1.25
center=0.75
lfe=0.75
rear=0.75
rear2=0.70
```

Edit it, then reboot — or, for an immediate change, delete `/tmp/gstcool.conf` and re-run
`/var/lib/webosbrew/dts_restore/init_dts.sh`.

## Dolby Vision toggle

Hybrid DV+HDR MKVs play as Dolby Vision by default. To force HDR instead (e.g. if DV shows a
black screen after seeking — see [Troubleshooting](#troubleshooting)), create `/tmp/dv_disable`.
The flag is checked at each playback start (no reboot needed) but **resets on reboot** because
`/tmp` is volatile. A remote-button toggle can be wired via
[magic_mapper](https://github.com/andrewfraley/magic_mapper) using
[this gist](https://gist.github.com/pbatard/ea04494c0de63cd5d38b1f607ef64fbd).

---

## Troubleshooting

- **"This video does not support audio" on the first play after boot** — close the video and
  play it again. The LG player is sometimes slow to re-detect DTS. (Reportedly no longer occurs
  on newer webOS.)
- **DV+HDR MKV shows a black screen after seeking / resuming** — the file's DV mastering is
  incompatible with LG's DV engine on a non-zero start. Use the `/tmp/dv_disable` switch to play
  it as HDR.
- **A hybrid DV file plays as HDR, not DV** — its DV configuration is incompatible with LG's
  implementation; forcing DV would not work anyway.
- **Installer aborts under telnet** — use **ssh**; telnet is missing `GST_REGISTRY_1_0`.
- **"Cannot install while the media player is running"** — close any playing video and retry.
- **Check what happened** — the boot hook logs to `/tmp/dts_restore.log`.

---

## Limitations

- **Stereo (2.0) downmix only** — no multichannel, no passthrough. LG routes multichannel and
  bitstream passthrough exclusively through its **proprietary** audio decoder/sink; open decoders
  (including the one used here) can only reach a stereo-limited sink. Stock multichannel FLAC hits
  the same wall on unmodified firmware. Passthrough to an AVR is out of reach without reverse-
  engineering LG's proprietary libraries. *(An experimental route to real multichannel is being
  explored — see [Experimental: multichannel](#experimental--extras) below.)*
- **Root required.**
- **4K content cannot 2× fast-forward** — a stock LG limitation that also applies to AC3/AAC.

## Changelog (reconciling the issue-#72 timeline)

- **Dec 2022 (pre-release hacks):** swapped in *vanilla upstream* GStreamer libs — this lost 2×
  playback on all MKVs and had various DTS edge cases. **These limitations no longer apply.**
- **v3 (Dec 2022):** fixed floating-point-output DTS and rear-channel downmix ordering.
- **v1.0 (Jan 2023):** rebuilt the libraries from **LG's released source**, restoring 2× playback
  (it even works for DTS, except on 4K) and making the install permanent via the boot hook.
- **v2.0 (Nov 2025):** added `.mp4` DTS and Matroska Dolby Vision support.

## Building the libraries from source

The overrides are built from LG's published GStreamer sources under the
[`lgstreamer`](https://github.com/orgs/lgstreamer/repositories) org (released after an LGPL
compliance request), using the WebOSBrew SDK
([`meta-lg-webos-ndk`](https://github.com/webosbrew/meta-lg-webos-ndk/releases)). Per-`.so`
provenance:

- `libgstmatroska.so`, `libgstisomp4.so`, `libgstisomp4_1_8.so` — `lgstreamer/gst-plugins-good`
  @ `lg` (1.14.4): Matroska/mp4 DTS demux re-enabled + Matroska Dolby Vision added.
- `libgstlibav.so` — `lgstreamer/gst-libav` @ `lg` (1.14.4): DTS (dca) decode with forced stereo-
  integer downmix and `[downmix]`-coefficient support.
- `libgstmpegtsdemux.so` — `lgstreamer/gst-plugins-bad` @ `lg` (1.14.4): MPEG-TS/BD DTS demux
  (for `.m2ts`). *Optional — the boot hook mounts it only if present in `gst/`.*

---

## Experimental & extras

This fork adds two work-in-progress components alongside the core tool:

- **`gst-dtstolpcm/`** — an experimental GStreamer plugin that converts DTS to BluRay LPCM so it
  rides LG's proprietary **multichannel** sink (real 5.1/7.1 instead of stereo downmix). See its
  own README for the design, build, and the on-TV test that gates the approach. **Not yet
  validated on hardware.**
- **`dts-enabler-app/`** — a "DTS Enabler" webOS homebrew app: a GUI to enable/disable DTS
  restore, view status, tune the downmix, and toggle DV, installable from the Homebrew Channel.
  **Scaffold — not yet packaged/tested on hardware.**

## License

GNU LGPL v2.1 or later (same as GStreamer and its plugins). **NOT endorsed by LG.** Provided
"AS IS" without warranty of any kind; the entire risk as to quality and performance is with you.
