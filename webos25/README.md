# DTS + TrueHD/MLP audio restore for webOS 25 (LG C5)

Restores **DTS** *and* **Dolby TrueHD / MLP** audio playback on a rooted LG C5 /
webOS 25 TV. Both codecs are **verified working on a real LG C5**, persistent
across reboot (a boot hook re-applies everything). Reversibility: most changes
are **bind-mounts** over a stock file, undone by Disable/Uninstall — a reboot drops
the mounts but the boot hook re-applies them, so only Disable/Uninstall turns it off —
the one exception is the GStreamer plugin registry, a persistent `cp -f`
reverted by regenerating a clean stock registry — and it only applies to a TV
whose stock plugins match a **verified set**, refusing (with an explicit
experimental opt-in) otherwise. See
[Compatibility gate, reversibility, and self-heal](#compatibility-gate-reversibility-and-self-heal)
below.

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

The app package also contains an **app-only experimental C2/G2 profile** named
`webos22-o22-gst118`. It is selected only for the exact analyzed global `W22O`
identity, firmware, ABI, GStreamer version, and stock plugin SHA-256 set; even then,
Enable requires the explicit two-step experimental opt-in and the UI says hardware
verification **NO**. It restores only the legacy MKV/MP4 DTS path (no TS/M2TS,
TrueHD, gain controls, or A/B compare). The webOS-25 CLI does not install this
profile. See [`app/README.md`](app/README.md#webos22-o22-gst118--exact-firmware-legacy-override-experimental-not-hardware-verified)
and [`docs/FIRMWARE-COMPATIBILITY.md`](docs/FIRMWARE-COMPATIBILITY.md#implemented-c2-app-policy-version-260).

## Folder layout

- `restore/` — the CLI tool: prebuilt decoders (`out/`, `truehd-out/`) + container
  demuxers (`demux-out/`) + `install.sh`/`uninstall.sh` + the `build*.sh` scripts to
  rebuild them (Docker).
- `app/` — the "DTS Enabler" webOS homebrew app (GUI enable/disable/uninstall).
- `docs/` — design notes (`MULTI-MODEL.md`), firmware evidence and proof labels
  (`FIRMWARE-COMPATIBILITY.md`), the target-detection probe (`detect-target.sh`),
  background (`WEBOS25-DTS.md`), and `experimental/`.

## Make-up gain + DRC (DTS/TrueHD quieter and less dynamically managed than native)

Both custom decoders now apply a tunable make-up gain so DTS/TrueHD match LG's
native AAC/AC-3/Atmos loudness, **plus** a dynamic range compressor (DRC
presets Off/Light/Medium/Night) and a dialogue (centre-channel) boost that
mirror LG's own Dolby DRC parameter model — LG's DSP applies Dolby Line-mode
DRC to native content by default but none to DTS, which is the real reason
dialogue is harder to follow on DTS/TrueHD than on native Atmos. All of it is
tunable from the app, no rebuild needed — including an **in-app A/B compare**
that renders the bundled DTS sample twice (DRC off vs. your saved settings)
and reports a measured dB delta, so you don't have to trust your ears alone.
See
[`docs/WEBOS25-DTS.md#loudness--make-up-gain`](docs/WEBOS25-DTS.md#loudness--make-up-gain)
for the mechanism (including the DRC model and the LG evidence behind it) and
[`restore/TUNING-RUNBOOK.md`](restore/TUNING-RUNBOOK.md) for the by-ear tuning
steps and the test/rebuild/verify/release loop.

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
  The legacy CX payload is also ELF32 ARM EABI5 soft-float, but targets GStreamer
  1.14 and is incompatible with this 1.24 runtime.

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

4b. **`adecswitch` (same-family stream-switch bin)** — `libgstadecswitch.so`
   (compiled rank 320) is staged in `/var/lib/webosbrew/dts25/` alongside
   `dtsdec`, and `adecswitch=320` is added to the same `[sw_decoder]` section of
   `gstcool.conf`. It fronts Dolby (AC-3/E-AC-3), TrueHD/MLP and DTS caps ahead
   of `decproxy` (rank 300) so `decodebin3` keeps one bin across a same-family
   stream switch instead of tearing the hardware decoder down mid-switch, while
   still handing Dolby off to `decproxy` for the actual decode. Because uMS sends
   the `acquired-resource` grant only once per pipeline, and stock `decodebin3`
   used to earn a re-grant by removing and recreating the decoder, the bin caches
   that event and replays it into every `decproxy` it plugs later; without the
   replay a Dolby decoder created after a track switch stays a `fakeadec` puppet,
   emits nothing and stalls the pipeline.

   **A `decproxy` is never taken to NULL while playback is running.** Switching
   away from Dolby parks it instead: unlinked and locked, but still alive and
   still owning its DSP decoder, and the next switch back reuses it. Taking one
   to NULL takes over a second on a C5, and any flush landing inside that window
   kills audio DSP0 in `eform_flush()` — which a seek does, and so does the video
   sink, which forces a pipeline flush whenever the video codec changes while
   PAUSED. Parking removes that operation from playback entirely rather than
   trying to time it; the parked decoder is released when the bin leaves PAUSED.
   A live `decproxy` absorbs a flush without complaint.

   The rank line is also a config-level kill switch: setting it to `0` reverts to
   stock `decodebin3` behaviour without touching the boot script.

5. **Registry** — the media GStreamer registry is regenerated (with
   `LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs` and a plugin path that
   includes `/var/lib/webosbrew/dts25`) so it contains `dtsdec`, `avdec_truehd`
   and `adecswitch` alongside the container demuxers, then written to
   `/mnt/flash/data/gst_1_0_registry.arm.bin`. See "Compatibility gate,
   reversibility, and self-heal" below for exactly what gates that write.

## Show DTS tracks in apps (separate opt-in)

Some apps decide whether to offer a DTS track from a capability string the TV
reports, `tv.model.edidType`, rather than from what the pipeline can decode.
Stremio hides DTS outright unless that string mentions `dts` (it reads the key
over `luna://com.webos.service.config`), and Kodi uses the same key for
`SupportsDTS()`. On a C5 it reads `TrueHD`, so DTS tracks never appear even with
the patch active and working.

The app's **Show DTS tracks in apps** card flips it to `TrueHD+dts`. It is
deliberately **not** part of Enable and defaults to off, because `arccontroller`
builds the EDID SADs the TV advertises over eARC from the same value and
`extinput` gates HDMI-input DTS on it — so it also changes what a connected
receiver is told this TV accepts on its own inputs.

**Known risk, and why this opt-in is off by default.** Applying it restarts configd,
which `audiooutputd` `Requires=`. On a C5 that service does not always survive the
restart: measured 2026-09-17, it **aborted** (`Main process exited, code=killed,
status=6/ABRT`), systemd brought it back, but the audio context was gone. Every pipeline
created afterwards logged `lgadec ERR: audio device is not opened` and played silent,
and the volume keys looked dead because the key arrived and the OSD appeared with
nothing left to apply it. Restarting `audiooutputd`, `audiod` and `umediaserver` by hand
did **not** recover it; only a reboot did. GitHub issue #5 reports the same shape on a
G5 (webOS 11.2.0) where Home and Mute die instead, which fits: on webOS 11 the key
handlers are themselves configd clients. It is intermittent and only reachable with this
opt-in on. DTS and TrueHD playback are unaffected either way.

**There is no way to make it stick without that restart.** `tv.model.edidType` comes
from the configd layer `/var/run/tvconfig/lls` (priority 153), regenerated every boot by
`lowlevelstorage` from a raw eMMC partition. Every layer that outranks it is either on
the read-only rootfs overlay (`tooltype` 155, `product` 190, `broadcast` 195,
`tvoverlay` 199, and `/mnt/platform-plugins` 156, which is `ro` even though the path is
absent) or on tmpfs wiped at boot (`rmm` 154, `remote` 299). configd re-parses the layer
dirs at boot rather than trusting its cache, so a patched cache does not survive either.
The only persistent source is the factory partition, which is exactly what the rooting
projects warn never to write.

**Do not make this apply live.** It was tried in `webos25-2.37` and withdrawn.
Applying it means restarting configd, and on a C5 that does not stop `audiooutputd`
cleanly: it **aborts** (`Main process exited, code=killed, status=6/ABRT`). systemd
restarts it, but the audio context is gone, every pipeline created afterwards logs
`lgadec ERR: audio device is not opened` and plays silent, and the volume keys appear
dead because the key arrives and the OSD shows but nothing is left to apply it.
Restarting `audiooutputd`, `audiod` and `umediaserver` by hand does **not** recover it;
only a reboot does. Measured on a C5 (webOS 10.3.1) on 2026-09-17. The same restart also
runs at boot, which is where it is least harmful because nothing is playing yet, but it
is the same hazard.

**Both directions record intent and take effect at the next restart.** Turn on
writes the marker file (and refreshes the installed boot script); Turn off removes
the marker and drops the bind. Neither touches configd, so neither can hang the app:
with the old `systemctl restart` the apply blocked for ~90s (see below), longer than
the Homebrew exec bridge waits, and the UI sat on "turning on…" forever. The boot
hook does the actual apply, verified end to end across a reboot. A **Restart TV
now** button appears whenever a restart is the remaining step in either direction.

`edidType` is factory data, not a rootfs config file: `lowlevelstorage` writes it
into `/tmp/var/run/tvconfig/lls/factorydb.json` (tmpfs) and configd folds that in
as its "Low-Level Storage Info" layer. The boot hook binds an edited copy over
that file, changes the one `edidType` string inside
`/var/preferences/configd_db.json` and restarts configd. Both are rebuilt at
boot, so **removing the marker plus a reboot is a complete revert**, and the marker
file `/var/lib/webosbrew/dts25/appdts.enabled` is what makes the hook re-apply it.

**Patch that cache, never delete it.** A missing cache makes configd rebuild its
whole configuration from the layer dirs and republish every value, and the OLED
panel settings share the `tv.model` blob with `edidType` (`defaultStdBacklight`,
`digitalEye`, `eyeCurveDerivation`, `eyeSensorLEDGain`, `oledCPC`,
`supportOledOffRsQuickStart`). Measured on a C5: with the cache present a configd
restart logs **zero** `parseFiles` lines, so patching the single string changes one
key and leaves every other value byte-identical. (An owner reported the panel
dimming after screen-off while the cache was still being deleted; that has not been
re-tested since, so it is a suspect, not a confirmed cause.) Uninstall copies the
pristine value back out of the factory file the same way; Turn off just removes the
marker and lets the next boot re-read the stock file.

**Applying it restarts configd, and how that restart is done is the whole story.**
Ten units `Requires=configd.service`, among them `pqcontroller` (picture),
`videooutputd`, `audiooutputd` and `umediaserver`. A plain `systemctl restart` queues
restart jobs for all of them, configd's own job waits ~90s behind that cascade, and
`pqcontroller` coming back is the "Auto Power Save for a minute and a half after every
boot" an owner reported. Measured on a C5 with a 1-second sampler of systemd's job
queue. Two other things compound it: configd ignores SIGTERM once it has subscribers,
so the stop alone eats the 90s `TimeoutStopUSec`; and `systemctl kill` on its own trips
`Restart=on-failure`, which cascades the same way.

It also **waits for boot to finish first**. The Homebrew hook runs about 30s in
while `bootmode-normal-boot-done` only goes active around 45s, so the apply was
restarting a core service with systemd still bringing the system up. An owner
reported the remote's Home button dead after every boot with the opt-in on, and it
survived both the cascading restart and the isolated one, which points at the timing
rather than at what else gets restarted. The wait is bounded and applies anyway if
the signal never arrives.

Why a restart at all: configd starts at 2.41s and `/var` mounts at 2.47s, so its
cache is genuinely unreadable that early (`Cache file is accessible (0)`) and it
always re-parses the layer dirs. There is no way to have the value in place before
its first read.

The hook therefore restarts configd **alone**:

```sh
systemctl --job-mode=ignore-dependencies --no-block stop configd.service   # configd only, returns at once
systemctl kill -s KILL configd.service                                       # so that stop completes now
systemctl --job-mode=ignore-dependencies start configd.service              # configd only
```

A requested stop does not trigger `Restart=on-failure`, and `ignore-dependencies` keeps
every other unit out of the job queue. Measured at boot: hook at 36s, applied at 39s,
configd back in **1s**, and `pqcontroller` / `videooutputd` / `audiooutputd` /
`umediaserver` never restarted. The new configd loads its cache (zero `parseFiles`) so
it serves the patched value from the first request. `setConfigs` would have avoided the
restart entirely, but it sits behind the `configd.internal` ACG group, which
`/usr/share/luna-service2/allowed_groups.json` grants to no trust level a homebrew
service can hold.

A boot-ordered systemd unit would avoid the restart entirely, but there is nowhere
to put one: `/etc` and `/lib/systemd/system` are read-only squashfs, and the only
writable unit directory is `/run/systemd/system`, which is tmpfs and gone before
the next boot reads it.

Two things measured on a real C5 that are easy to get wrong:

- The higher-priority `/var/run/tvconfig/remote` layer declared in `layers.json`
  looks like the natural place for an override, but its selector is empty on
  these sets, so configd logs `(Remote) : ReadOnly Type (Skipped)` and never
  reads it.
- `/var/run` is a symlink to `/tmp/var/run`, so `/proc/mounts` records the bind
  under the **resolved** path. Guards written against `/var/run/...` never match,
  which stacks a new mount on every boot and never unmounts on Disable.

This only changes which tracks apps offer. It is not a decoder change.

## Compatibility gate, reversibility, and self-heal

**Verified TV sets.** Before binding anything, both the boot hook and Enable check the
live md5 of the three stock plugins we shadow (`libgstlibav.so`, `libgstisomp4.so`,
`libgstmpegtsdemux.so`) against a table of verified sets keyed on those hashes plus the
GStreamer major.minor version:

| Set | GStreamer | stock `libgstlibav.so` md5 | stock `libgstisomp4.so` md5 | stock `libgstmpegtsdemux.so` md5 |
|---|---|---|---|---|
| LG C5 OLED77C51LA (webOS 10.3.1) | 1.24.0 | `0fd6d65ac9e3a78b393a615eaff8ac0b` | `57fe57060774f248c05af5a411fc9a8f` | `9b84a95cf29bc025553c7dee829b7cc1` |
| LG G5 OLED77G55LW (webOS 10.3.1) | 1.24.0 | `0fd6d65ac9e3a78b393a615eaff8ac0b` | `57fe57060774f248c05af5a411fc9a8f` | `9b84a95cf29bc025553c7dee829b7cc1` |
| LG G2 OLED77G26LA/OLED77G29LA (webOS 10.3.1) | 1.24.0 | `0fd6d65ac9e3a78b393a615eaff8ac0b` | `cf4d9bb9e3c3ad83f1a75a399d2f0b93` | `772fb3b29e224423035eec9e93615b23` |
| LG C2 OLED55C21LA (webOS 10.3.1) | 1.24.0 | `0fd6d65ac9e3a78b393a615eaff8ac0b` | `cf4d9bb9e3c3ad83f1a75a399d2f0b93` | `772fb3b29e224423035eec9e93615b23` |

A TV whose stock hashes are **not** in the table is refused by default — Enable/the boot
hook show the probed values so they can be reported for a future entry — with an explicit
two-step **"Try anyway (experimental)"** opt-in that applies the override only if the
payload's own dynamic dependencies actually resolve on that TV. This is deliberately **not**
a soname-equality check against stock: the verified C5's **stock** `libgstlibav.so` links
**ffmpeg 5.x** (`libavcodec.so.59`, `libavformat.so.59`, `libavutil.so.57`,
`libavfilter.so.8`, 145352 B, md5 `0fd6d65ac9e3a78b393a615eaff8ac0b`), while **ours** links
**ffmpeg 4.4** (`.58`/`.58`/`.56`/`.7`) resolved through
`RUNPATH=/var/lib/webosbrew/truehd/libs` — a check that demanded matching stock sonames
would reject the very TV the payload is verified on. Enabling therefore moves gst-libav's
software decoders from LG's ffmpeg 5 build to our ffmpeg 4.4 build.

**Firmware-drift stand-down.** `/var/lib/webosbrew/dts25/stock.fp` records, from the last time the
gate passed, the pristine hashes of the three plugins we shadow **and** of the two live `/etc` files we
bind snapshots of (`device_codec_capability_config.json`, `gstcool.conf`). If a firmware update changes
any of the five, the boot hook stands itself down — toast, nothing bound — instead of applying a payload
verified against a stock file the TV no longer has. The `/etc` pair is in there because those snapshots
are derived at install time and only change via OTA: without them, an update that rewrote only
`gstcool.conf` would keep the verdict `verified` while the hook quietly reverted LG's own config change,
system-wide, indefinitely. Drift is therefore evaluated **before** the verified-sets table match — the
table keys on the plugin hashes and cannot express `/etc` state, so "has this TV changed since we
recorded it" outranks "does this TV look like a known-good one". Protection engages from the first apply
under a build that records those keys; an older `stock.fp` that never had them does not read as drift.

One residual, stated rather than engineered around: `libgstmatroska.so` is neither shadowed nor
fingerprinted, so an OTA changing its `A_DTS` retag would silently lose MKV DTS. That fails in the
acceptable direction — it costs our codec and harms nothing else — and the registry commit gate still
passes, because it checks that `matroskademux` registers, not what caps it emits.

**Registry commit gate.** After binding, the regenerated registry is only copied over
`/mnt/flash/data/gst_1_0_registry.arm.bin` if `dtsdec`, `avdec_truehd`, `adecswitch`,
`qtdemux`, `tsdemux`, **and** `matroskademux` all survive the scan; if any is missing, the
binds are dropped instead and the TV is left on its stock registry.

**Self-heal on removal.** Removing the payload (app or CLI) while still enabled no longer
leaves a dangling override: at the next boot, finding neither the app's install directory
nor `/var/lib/webosbrew/dts25/.cli-install`, the hook drops every bind, regenerates the
clean stock registry (the same routine `uninstall.sh` step 2b uses), removes the state
directories, and unlinks itself. `.cli-install` is written by `install.sh` so an SSH/CLI
install is never healed away by mistake; the app's Enable removes it, so whichever surface you last
used to manage the install is the one that owns it.

**A refused install reverts, it does not stop half-way.** Both installers drop existing binds before
they measure (so the fingerprints they read are pristine), which means a refusal on a TV that *was*
enabled would otherwise leave it with the binds gone but our registry still live. Every refusal branch
therefore stands the TV down properly — binds dropped, stock registry regenerated if one of ours was
live — and the message says what happened rather than claiming nothing changed.

**Disable and Uninstall can report a deferral.** Both are gated on the stock-registry rebuild
succeeding. If it fails, the app answers with `registryReverted: false` (and `uninstallDeferred: true`
where files were kept), the staged files stay put, and the UI says so instead of printing "registry
restored to stock" — because in that state our decoder may keep working until the registry is rebuilt.
`uninstall.sh` prints the same thing as `INCOMPLETE` and asks for a re-run. A revert that did not happen
is never reported as a clean one; the same applies to an override that could not be detached even
lazily, which surfaces as `unmountWarning`.

**Cleanup is deferred rather than half-done.** The heal regenerates LG's registry *first* and deletes
our files only if that succeeded. That regen is a cold-cache full plugin scan under `timeout`, running at
boot — the busiest moment on the box — and if it times out, deleting the plugins anyway would leave the
live registry pointing at files that no longer exist, which is exactly what broke other apps' audio on a
real C5 on 2026-07-23. So on failure the hook keeps the binds dropped, keeps the state, keeps itself
installed, toasts that cleanup was deferred, and retries at the next boot. `uninstall.sh` follows the
same rule: if it cannot rebuild a stock registry it says so loudly and leaves the files in place for a
re-run, rather than reporting a clean uninstall it did not achieve.

**An incomplete install is refused, not deleted.** Self-heal only fires when nothing owns the
install any more (no app directory *and* no `.cli-install`). A payload that is merely incomplete —
`libgstdtsdec.so` or `libgstlibav.so` missing — is a different case: the hook binds nothing, deletes
nothing and keeps the boot hook, so re-opening the app or re-running `install.sh` repairs it. It does
repair one thing: if a registry *we* wrote is still live while our plugins are gone (the `cp -f`
registry outlives our files, which is what broke other apps' audio on a real C5 on 2026-07-23), it
regenerates the stock registry. The two container demuxers stay **optional** — a build without
`demux-out/` is a normal MKV-only install, not a fault.

**Forcing from the CLI.** The app's two-step "Try anyway (experimental)" only ever offers itself for
an `unverified` verdict. The CLI equivalent is explicit:

```sh
FORCE=1 sh install.sh     # apply on an unverified set, recording forced=1 in stock.fp
```

Like the app, `FORCE=1` only ever applies to an `unverified` verdict — it can never override a
**drift** verdict (stock plugins changed since the last successful apply) or a GStreamer major.minor
change: both stand the install down unconditionally regardless of `FORCE`, which is the fail-safe
against a firmware update the payload was never checked against. That is not a dead end: Uninstall
removes `stock.fp`, so **Uninstall then Enable** puts the TV back into the `unverified` flow, where the
ordinary two-step opt-in applies — the same explicit consent, without a special case for drift. Reporting
the new fingerprints so the set can be added to the table is the durable fix.

**Read-only preflight.** `W25_CHECK=1 sh /var/lib/webosbrew/dts25/init_dts25.sh` runs the whole gate
and prints `VERDICT=`, `REASON=`, `LABEL=`, `CANFORCE=`, `LOADER=`, `LOADER_STAGED=`, `GST_MM=`,
`PRODUCT_ID=`, `WEBOS_RELEASE=` and the measured `MD5_*` values, without mounting, copying or writing
anything. (`REFUSED=`/`REASON=` are what the *apply* path prints when it stands down — don't parse for
`REFUSED=` in check mode.) `install.sh` and the app's Enable both use it rather than duplicating the
gate; the boot hook runs the same gate inline, from the same shared block. One caveat worth knowing:
the *installed* script is only rewritten by Enable or `install.sh`, so a TV enabled under an older app
keeps that script — and its verified-sets table — until Enable is pressed again. The app reports
`hookStale` when its embedded copy is newer than the installed one; pressing Enable refreshes it. The
installed script carries a gate-version stamp, and `detect`/`status` expose `hookStale`,
`hookStaleReason`, `hookGateVersion`, `appGateVersion`, `hookScriptInstalled` and `hookNewer`. The
comparison is numeric and directional: an installed script *newer* than the app (a CLI tarball ahead of
the Homebrew Channel, which is normal — the two tracks are independent) reports `hookNewer` and advises
updating the app, never "press Enable", because pressing Enable there would overwrite the newer gate
with the older one. A deliberately Disabled TV is not nagged either. The gate stamp shipped with this
release is `2`. The stamp is versioned
independently of the app version on purpose: a cosmetic app release must not invalidate a current hook,
and a gate change must not hide behind an unchanged app version. The app additionally md5-compares what
Enable *would* write against what is installed, so an un-bumped stamp is still caught.

**Reversibility, precisely.** Most of the mechanism above is a **bind-mount** over a stock
file, undone by Disable/Uninstall. Note a reboot does *not* disable anything: it drops the
mounts, then the boot hook re-applies them. Only Disable/Uninstall removes the hook. (A
half-applied Enable is the exception — its recovery marker survives, so the next boot
detaches and refuses instead of retrying.) The one exception is the GStreamer
plugin registry: it is written with a persistent `cp -f`, not a bind, so Disable/Uninstall
explicitly regenerate a clean stock registry from the pristine on-disk plugins to revert
it (`uninstall.sh` step 2b). A bind can also be **busy** at Disable time — on the C5,
`umount /usr/lib/gstreamer-1.0/libgstlibav.so` returns `target is busy` because
`WebAppMgr` (pid 3492) has it mapped live; the fallback is a lazy detach (`umount -l`), and
existing mappings finish out against our lib until a reboot clears them fully.

**System-wide reach is the point, not an accident.** The overrides sit at the rootfs
GStreamer plugin paths, so every app jail that maps them sees ours too — measured on the
C5: 27 jail-side binds per shadowed library (28 counting the rootfs one), and both
Netflix's and the browser's jail views hash to our libraries. That is DTS/TrueHD working
anywhere the media pipeline is used, which is the point of the app — not a leak.
Jail-side binds are deliberately left alone by Disable/Uninstall (detaching them would
break that jail's own view); a jail picks up stock again on its own next restart, or on a
TV reboot.

**Also:** the registry regen still runs in-process under `timeout` so a hung scan cannot
trip the Homebrew Channel failsafe, and every refusal/abort path posts a fail-safe toast
and `exit 0`s.

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
and normal AAC mp4 playback is unaffected.

**TrueHD containers:** **MKV and `.ts`/`.m2ts` are supported; `.mp4` is not.** TrueHD in
MPEG-TS needed its own fix: separately from DTS, LG wraps the BluRay TrueHD stream-type case
in `tsdemux.c` in `#if 0` and falls through to `goto done`, so stream_type `0x83`
(`ST_BD_AUDIO_AC3_TRUE_HD`) was silently dropped and the pad never exposed. What actually
decoded was the **AC-3 compatibility substream carried on the same BD PID** — which is why
TrueHD in `.ts`/`.m2ts` "played fine" while not being TrueHD at all. LG's own comment gates it
on *"until we have ability to decode this codec"*, and this payload ships `avdec_truehd`, so
`build-demux.sh` un-`#if-0`s the case (the `target_pes_substream = 0x72` inside it is what
selects the TrueHD substream over the AC-3 core). Verified on the C5: a real BD m2ts carrying
TrueHD 5.1 + AC-3 previously exposed only the AC-3 tracks, and now reports
`audio: Dolby TrueHD, Channels: 6 (FL FR FC LFE SL SR)` decoding to
`audio/x-raw, S32LE, 6 channels, 48000 Hz` — the side-pair channel mask (`0x0c0f`) rather than
AC-3's rear-pair (`0x003f`) proving it is the TrueHD substream. DTS in `.ts`/`.m2ts` re-checked
unchanged. **`.mp4` TrueHD remains unsupported** — `qtdemux.c` has no TrueHD/MLP codepath at all
(no `mlpa` fourcc handling), so it needs new code rather than a gate flip.

**Caveats (honest):**
- **Discrete 5.1 reaches LG's sink — confirmed in real playback, no downmix in the pipeline.**
  Measured on a real C5: `dtsdec` emits native discrete 5.1 (6 channels of distinct content) as
  S32LE/48 kHz, matching a reference DTS core decoder within ~0.1–0.2 dB per channel. During actual
  Media-Player playback the GStreamer debug log shows LG's `audiosink` negotiating
  `audio/x-raw, S32LE, 48000, channels=6` (its sink pad advertises `channels=[1,10]`), so full 5.1
  PCM is delivered end-to-end to LG's audio HAL — there is **no stereo downmix anywhere in the
  GStreamer path** (unlike the CX/upstream tool, which force-downmixes to 2.0). A BD-LPCM re-frame is
  therefore **not needed** to reach a multichannel sink. **The only remaining variable is the TV's
  own output stage:** internal speakers fold 5.1 into the built-in array, while **HDMI eARC to an AVR**
  carries the multichannel PCM subject to the "Digital Sound Output" setting. Optical/S-PDIF is a
  two-channel PCM link, so it cannot carry 5.1 from a decode-to-PCM path at all — eARC is the only
  multichannel route out. Confirm 5.1 on an AVR's input display; this half is the TV's routing, not
  something this project measures.
- **DTS-HD:** the shipped `dtsdec`/`libdca` decodes the DTS **core** only — not the DTS-HD MA
  lossless (XLL) extension, and not the DTS:X extension substream. (ffmpeg's XLL-capable `dca`
  decoder is deliberately not built; see `build-demux.sh`/`build-truehd.sh`.) So a DTS:X or
  DTS-HD MA 7.1 title decodes as its 5.1 core.
- **TrueHD Atmos:** the **full base bed decodes** — measured on a C5, a real
  `Dolby TrueHD + Dolby Atmos` 7.1 MKV yields `audio/x-raw, S32LE, 48000, channels=8`
  (`channel-mask=0x0c3f`) with no substream or downmix warnings. Only the **object layer** is
  dropped, which no open decoder renders.
- **No object audio, and no "Dolby Atmos"/"DTS:X" badge** for DTS or TrueHD — and this is not an
  AVR or eARC limitation: the badge appears on TV speakers alone for AC-3, because LG's
  `libgstlgaudiodec.so` is the only element with an Atmos codepath and its sink caps accept
  neither `audio/x-dts` nor `audio/x-true-hd`. Structural, not configurable. See
  [`docs/PASSTHROUGH.md`](docs/PASSTHROUGH.md).
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
sh uninstall.sh     # unmounts all binds (capability, gstcool, libav, isomp4,
                    # mpegtsdemux — with a lazy-detach fallback if one is busy),
                    # regenerates a clean stock registry, removes the state dirs + hook
```

A reboot after uninstall guarantees a fully clean state.
