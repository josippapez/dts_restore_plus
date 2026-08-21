# dts_restore

Restore **DTS audio playback** (and add **Matroska Dolby Vision** support) on rooted LG webOS
TVs, where LG deliberately removed DTS decoding in firmware.

It works by bind-mounting recompiled GStreamer libraries — built from **LG's own released
sources**, with DTS demux/decode re-enabled — over the "nerfed" ones, and raising the DTS
decoder's priority. No stock firmware file is modified in place: on the original **CX** tool
everything is a bind-mount that a full power-off reverts; the **webOS 25** build reverts the
same way except for the GStreamer plugin registry, which is a persistent copy undone by
Disable/Uninstall regenerating a clean stock one — see
[webOS 25: compatibility gate, reversibility, and self-heal](webos25/README.md#compatibility-gate-reversibility-and-self-heal).

> Background and the full development history are in
> [RootMyTV issue #72](https://github.com/RootMyTV/RootMyTV.github.io/issues/72).

---

## webOS 25 (LG C5/G5) — install the app from the Homebrew Channel

On a rooted **webOS 25** TV, the easiest install is the **DTS Enabler** app. In the
Homebrew Channel go to **Settings → Add repository** and paste this URL:

```
https://josippapez.github.io/dts_restore_plus/api/apps.json
```

Then install **DTS Enabler** from the list — it restores **DTS + Dolby TrueHD/MLP**
(incl. DTS in `.mkv`/`.mp4`/`.ts`/`.m2ts`) with Enable/Disable/Uninstall, a self-test,
and play-by-ear. The `.ipk` is pulled from the release and **sha256-verified**;
updates are automatic. Prefer SSH? Use the CLI in [`webos25/`](webos25/README.md).

The same app package also exposes an **app-only, experimental C2/G2 DTS profile** for
the exact analyzed global `W22O` firmware. It reuses the legacy payload for MKV/MP4,
requires a two-step opt-in, and is **not hardware-verified**. Other C2/G2 firmware and
all B2/C3/B3/C4 families remain refused; see the
[firmware evidence](webos25/docs/FIRMWARE-COMPATIBILITY.md) and
[app profile contract](webos25/app/README.md#webos22-o22-gst118--exact-firmware-legacy-override-experimental-not-hardware-verified).

*(For OLED CX / webOS 3–6, see the sections below.)*

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
- `webos25/app/` — a "DTS Enabler" webOS homebrew app (GUI). **Install via the Homebrew Channel**
  by adding the repository `https://josippapez.github.io/dts_restore_plus/api/apps.json` (Settings → Add
  repository), then pick "DTS Enabler". `webos25/docs/` — design notes + the target-detection probe.
- **Loudness tuning:** DTS/TrueHD now also get a per-codec make-up gain, Dolby-style
  DRC presets (Off/Light/Medium/Night), and a dialogue (centre-channel) boost — all
  tunable from the app, with an in-app A/B compare that measures the difference on the
  bundled clip. Defaults are inert (bit-identical to the previous build). See
  [`webos25/docs/WEBOS25-DTS.md#loudness--make-up-gain`](webos25/docs/WEBOS25-DTS.md#loudness--make-up-gain)
  and [`webos25/restore/TUNING-RUNBOOK.md`](webos25/restore/TUNING-RUNBOOK.md).

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
on several later LG sets (per issue #72), even though extracted C2 firmware shows that not every
such set has a 1.14 stock runtime. There is a single library set — models outside the list below
can still install (the installer just warns first), using the same binaries.

| Model family | webOS | GStreamer | Status |
|---|---|---|---|
| **OLED CX** | 5.x | 1.14.4 | **Reference target** |
| OLED BX | 5.x | 1.14-class | Same generation as CX |
| OLED C1 / G1 | 6.x | 1.14-class | Community-confirmed |
| OLED C2 / G2 | "22" | stock **1.18.2**; legacy payload 1.14.4 | The root CX installer still only warns and applies its one legacy recipe. DTS Enabler 2.6.0 instead offers a fail-closed, exact-firmware **experimental opt-in** for global `W22O` `04.40.93`/`04.40.93.01`; firmware/QEMU evidence only, **not hardware-verified** ([firmware evidence](webos25/docs/FIRMWARE-COMPATIBILITY.md)) |
| NanoCell / LCD (UN7xxx, NANO7xx, 2020–2021) | 5.x / 6.x | 1.14-class | Community-confirmed |
| webOS 22 / 23 / 24 (C3/C4, G3/G4, Realtek B2/B3) | 22 / 23 / 24 | stock **1.18.2 / 1.18.5**; webOS 24 unextracted | B2/C3/B3 receive diagnostic refusal profiles; C4/G4 remains unknown. No forceable restore mechanism ([firmware evidence](webos25/docs/FIRMWARE-COMPATIBILITY.md)) |
| webOS 25 (C5/G5) | "10" | **1.24** | Not these binaries — needs a separate soft-float armel 1.24 build ([webos25/README.md](webos25/README.md)) |

## Persistence — what survives a reboot

| Applied | Survives reboot? | Reverted by |
|---|---|---|
| `/var/lib/webosbrew/init.d/restore_dts` boot hook | **Yes** | `uninstall.sh` |
| Library bind-mounts over `/usr/lib/gstreamer-1.0/` | No (re-applied each boot by the hook) | reboot / uninstall |
| `/tmp/gstcool.conf`, registry overlay, `/tmp/dv_disable` | No (`/tmp` is volatile) | reboot |
| GStreamer registry partition | Semi (regenerated to `/tmp` and overlaid) | full power-off |

---

## Tuning the stereo downmix

> **CX / webOS 3–6 only.** This applies to the CX tool's binaries, whose gst-libav forces a
> stereo downmix. The **webOS 25** build does **not** downmix — it decodes native discrete 5.1
> (verified on a C5), so there is no `downmix.conf` there. See
> [`webos25/README.md`](webos25/README.md).

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

> These limitations describe the **CX / webOS 3–6** tool (the binaries in `gst/`). The
> **webOS 25** build is different — it decodes **discrete 5.1 (no downmix)**; see its own
> [limitations/caveats](webos25/README.md#per-codec-status).

- **Stereo (2.0) downmix only *(CX tool)*** — the CX gst-libav forces a 2.0 downmix; it does
  not reach LG's multichannel sink. (This does **not** apply to the webOS 25 build, which emits
  discrete 5.1 — measured on a C5 to match a reference decoder within ~0.1–0.2 dB per channel.)
  Bitstream passthrough to an AVR is out of scope on either platform (decode-to-PCM only).
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
- **`webos25/app/`** — the shipping "DTS Enabler" Homebrew app: a GUI for the verified
  webOS-25 mechanism plus the exact-firmware experimental C2/G2 profile described above.

## License

This project's own code (installer scripts, the app, the service) is **GNU LGPL v2.1 or later**,
same as GStreamer and its plugins. The shipped binary payload is **mixed**, because one component
links a GPL library:

| Shipped artifact | License | Why |
|---|---|---|
| App, service, `install.sh`/`init_dts25.sh`/`uninstall.sh` | LGPL-2.1-or-later | this repo's own code |
| Legacy `gst/libgst{matroska,isomp4,isomp4_1_8,libav}.so` (packaged in `payload/cx/`) | LGPL-2.1-or-later | LG GStreamer 1.14.4 sources; shared by the inherited CX mechanism and experimental C2 profile |
| `webos25/restore/demux-out/libgst{isomp4,mpegtsdemux}.so` | LGPL-2.1-or-later | built from gst-plugins-good / -bad |
| `webos25/restore/truehd-out/libgstlibav.so`, `libav*.so*`, `libsw*.so*` | LGPL-2.1-or-later | ffmpeg 4.4 configured **without** `--enable-gpl` / `--enable-version3` (see `webos25/restore/build-truehd.sh`) |
| `libgstdtsdec.so`, `libdca.so.0` | **GPL-2.0-or-later** | the plugin source is LGPL, but it links **libdca**, which is GPL-2.0-or-later — the resulting binary is a combined work |

Each component keeps its own license; because the DTS decoder pair is GPL-2.0-or-later, any
distributed `.ipk` or tarball contains GPL-2.0-or-later code, and the corresponding-source offer
must cover it. The webOS-25 build scripts and patched sources are in `webos25/restore/`; provenance
for the separately packaged legacy LG 1.14.4 files is listed above and in
`webos25/app/payload/cx/README`.

**NOT endorsed by LG.** Provided "AS IS" without warranty of any kind; the entire risk as to
quality and performance is with you.
