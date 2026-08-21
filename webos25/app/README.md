# DTS Enabler (universal)

One webOS Homebrew app that restores DTS audio on rooted LG TVs **across
generations**. It detects the TV at runtime, picks the correct DTS-restore
mechanism for that generation, and offers **Enable / Disable / Uninstall** plus
a live **status panel** — no SSH, no hand-editing files, no guessing which
binary set to use.

This app **supersedes** the single-target `dts-enabler-app/` (which only knew
the CX demuxer-override mechanism and would have been actively harmful on a
webOS-25 TV — see MULTI-MODEL.md §3.1).

> Wraps the on-device work in `../restore/` (verified webOS-25 mechanism) and
> [`dts_restore`](https://github.com/lgstreamer/dts_restore) by Pete Batard (CX
> mechanism). Requires a **rooted** LG TV with the **Homebrew Channel**
> installed. Not endorsed by LG.

---

## The detect → branch model

The app never assumes a mechanism. On launch (and on every **Refresh**) the JS
service runs a **read-only** detection probe (the logic of
`../docs/detect-target.sh`, embedded as a constant) that measures the axes that
decide binary compatibility and fix strategy:

1. **On-device identity** — hardware/OTA ID, product ID, board type, firmware,
   and webOS release. These are mandatory parts of the C2/G2 gate.
2. **CPU arch / float ABI of the GStreamer userspace** — dynamic loader name +
   the ELF `e_flags` float-ABI nibble of a real `/usr/lib/gstreamer-1.0/*.so`
   (not `uname -m`: the C5 has an aarch64 kernel but a 32-bit soft-float
   userspace).
3. **GStreamer version** — `gst-inspect-1.0 --version` (drives plugin ABI).
4. **How LG disabled DTS** — which decoders are registered, and a static
   heuristic on `libgstmatroska.so` (A_DTS re-tag vs. demux nerf).
5. **Stock artifact identity** — the mechanism-specific hashes used to refuse
   firmware or plugin drift before any override is applied.

Those collapse to a **profile**, and the profile selects the mechanism:

```
PROFILE                         ACTION
webos25-armel-gst124            verified/experimental webOS-25 mechanism
webos22-o22-gst118              exact-firmware C2/G2 opt-in, hardware unverified
cx-armv7-gst114                 inherited CX demuxer override
webos22/23 diagnostic profiles  refuse with family-specific evidence
unknown / unknown-*             refuse cleanly
```

`enable` / `disable` / `uninstall` each **re-detect** (they never trust the
client) and dispatch to the matching mechanism's hardcoded command builder. An
unknown/unsupported profile is refused with a clear message — we never apply an
ABI-mismatched mechanism (CX 1.14 libs on a 1.24 TV would break MKV/MP4). The
diagnostic B2/C3/B3 profiles are deliberately non-forceable.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  DTS Enabler web app  (io.github.josippapez.dtsenabler)             │
│  index.html + js/app.js + css/style.css                      │
│  - D-pad navigable UI, status panel, Enable/Disable/Uninstall│
│  - sends NO free-form parameters (methods take none)         │
│         │ webOS.service.request / PalmServiceBridge          │
│         ▼                                                     │
│  JS service  (io.github.josippapez.dtsenabler.service)             │
│  service/service.js                                          │
│  - detects the profile (read-only probe)                    │
│  - branches to the matched mechanism's command builder      │
│  - all shell is author constants (empty injection surface)  │
│         │ luna://org.webosbrew.hbchannel.service/exec         │
│         ▼                                                     │
│  Homebrew Channel exec service  (runs as ROOT)              │
│         ▼                                                     │
│  webOS 25: stage dtsdec+libdca, regen+bind media registry   │
│  C2/G2:    exact-gated legacy libs, dedicated owned state   │
│  CX:       bind-mount demuxer libs, bump avdec_dca, gstcool │
└──────────────────────────────────────────────────────────────┘
```

The app holds **no elevation of its own**. Every privileged action is a shell
string sent to the Homebrew Channel (HBC) exec service via the hardened
`rootExec()` wrapper carried over from the single-target app.

---

## Per-profile mechanism

### `webos25-armel-gst124` — decoder-inject + demux-enable (VERIFIED)

Mirrors `../restore/install.sh` + `../restore/init_dts25.sh` exactly. Restores **DTS
(incl. mp4/ts/m2ts containers) and TrueHD/MLP (MKV + ts/m2ts; `.mp4` TrueHD is
not supported — `qtdemux` has no TrueHD codepath)**.

- **Enable:** re-detect, then run the same **compatibility gate** the CLI boot hook
  runs (see below) — refuse unless the TV's stock plugins match a verified set, or
  the caller opted into `force`. If the gate passes, stage three payloads — DTS
  (`libgstdtsdec.so` + `libdca.so.0` → `/var/lib/webosbrew/dts25/`), TrueHD
  (`libgstlibav.so` + ffmpeg libs → `/var/lib/webosbrew/truehd/`), and the container
  demuxers (patched `libgstisomp4.so` + `libgstmpegtsdemux.so`, `dts_support` default
  TRUE plus the BD TrueHD stream-type case un-`#if-0`d → `/var/lib/webosbrew/demux25/`).
  Generate the two `/etc` overrides
  (codec-capability TRUEHD/MLP; gstcool `avdec_truehd/mlp=310`). Write the
  canonical `init_dts25.sh`, which bind-mounts our libav, the demuxers, and the
  overrides, then regenerates the media GStreamer registry and — only if `dtsdec`,
  `avdec_truehd`, `qtdemux`, `tsdemux`, **and** `matroskademux` all survive the scan
  — writes it to `/mnt/flash/data/gst_1_0_registry.arm.bin`. Symlink the boot hook,
  apply now, restart `starfish-media-pipeline`.
- **Disable:** remove the boot hook, drop every bind (libav, demuxers, both `/etc`
  overrides — with a lazy-detach (`umount -l`) fallback if a target is busy, e.g. the
  live C5's `WebAppMgr` holding `libgstlibav.so` mapped), then regenerate a clean
  **stock** registry and `cp -f` it over the media one (the registry is a persistent
  copy, not a bind, so it is reverted this way rather than unmounted) → LG originals
  restored. Staged libs kept.
- **Uninstall:** disable + `rm -rf /var/lib/webosbrew/{dts25,truehd,demux25}`.
- **Test (self-check):** the `test` method decodes a bundled DTS sample per
  container (mp4/ts/m2ts) through the media registry and returns PASS/FAIL — an
  objective "is the patch working" check independent of the speaker/output stage.
  The UI also offers **play-by-ear** of the bundled samples (in-app `<video>`).

#### Compatibility gate, drift stand-down, and self-heal

`detect`/`status` report a compatibility **verdict** — `verified`, `forced`,
`unverified`, or `drift` — alongside `verdictReason`, `verifiedLabel`, `canForce`, and
the three measured plugin md5s, derived from the same verified-sets table, `stock.fp`
drift check, and self-heal routine documented in full (with the concrete measured
values) in
[`../README.md#compatibility-gate-reversibility-and-self-heal`](../README.md#compatibility-gate-reversibility-and-self-heal).
`enable` accepts `{force: true}` as a strict boolean — honoured only when the last
`detect` reported `canForce: true` — to drive the UI's explicit **"Try anyway
(experimental)"** opt-in; it is never interpolated into a shell command. Because the
gate, `stock.fp` drift stand-down, and the self-heal-on-removal boot behaviour all live
in the one canonical `init_dts25.sh` this app mirrors, they apply identically whether
the mechanism was installed via this app or via the CLI `restore/install.sh`.

With one timing caveat worth knowing: the script *installed on the TV* is only rewritten
by Enable (or the CLI installer), so a set enabled under an older app keeps that script —
and its verified-sets table — until Enable is pressed again. Rather than silently
rewriting a privileged script during a read-only detect, the service compares the
installed gate-version stamp against the one this build ships and reports `hookStale`,
`hookStaleReason`, `hookGateVersion`, `appGateVersion` and `hookScriptInstalled`; the UI
shows a note asking the user to press Enable. It also md5-compares what Enable would
write against what is installed, so an un-bumped stamp is still caught. On the CX profile
no compatibility verdict is reported at all, and the display is unchanged from before the
gate existed.

The **payload** has the same timing caveat, and needed its own check. An app update
replaces the bundled `payload/**` `.so` but never re-runs Enable, so the TV keeps decoding
with whatever it was last enabled with — and unlike the boot script the binaries carry no
version stamp to compare, so a stale decoder was previously invisible. Both copies live on
the TV (the bundle under the app directory, the staged copy under `/var/lib/webosbrew/`),
so the detect probe md5s them against each other — no hash has to be embedded in the app.
It reports `payloadStale`, `payloadStaleReason` and `payloadStaleFiles`, naming the files
that differ, and the UI shows a note asking the user to press Enable.

Six files are compared: `libgstdtsdec.so` and `libdca.so.0` (DTS), `libgstlibav.so` plus
`libavcodec.so.58` as the representative of the ffmpeg set — those libs move together, so
one is enough to catch a TrueHD payload swap without hashing a dozen files — and
`libgstisomp4.so` + `libgstmpegtsdemux.so` (container demuxers). A file this build ships
that is absent from the staged set is reported as missing rather than drifted (a partial
stage, e.g. a TV enabled under a build that shipped no demuxers), and a TV with nothing
staged at all is reported as neither — it was simply never enabled.

Like the hook-version check this **only reports**. It deliberately does not re-stage on
detect: doing so would re-apply a mechanism the user may have chosen to Disable, which is
the same reason the gate-version nag never rewrites the script by itself.

### Make-up gain & DRC control

DTS and TrueHD decode quieter than LG's native AAC/AC-3/Atmos, and neither
applies any dynamic range compression by default the way LG's own Dolby/DTS
decoders do — see
[`../docs/WEBOS25-DTS.md#loudness--make-up-gain`](../docs/WEBOS25-DTS.md#loudness--make-up-gain)
(mechanism) and its "Dynamic range compression (DRC) + dialogue boost"
subsection (the DRC model + the LG evidence behind it). The
`webos25-armel-gst124` profile's status panel has a **Make-up gain & dynamic
range** card (`index.html:65-161`) with three controls per codec (**DTS** and
**TrueHD**, each writing its own config file):

- **Gain** — dB stepper, range `[-20, +20]`, step `0.5`, default `0.0` (the
  original make-up gain, unchanged).
- **DRC preset** — cycles **Off → Light → Medium → Night** (`index.html:85-92`,
  `112-119`; cycling logic `stepPreset()`, `js/app.js:300-306`). Maps to the
  `drc`/`drc_boost`/`drc_cut` config keys per the epic's preset table; the
  service does the actual mapping and clamping, the app only displays the
  name.
- **Dialogue boost** — centre-channel dB stepper, range `[-10, +10]`, step
  `0.5`, default `0.0` (`index.html:93-100`, `120-127`; `stepCenter()`,
  `js/app.js:327-333`).

Both new controls share the existing **stepper** idiom (`[-] value [+]`,
`data-nav` spatial navigation, no `<input type=number>` so no on-screen
keyboard) already used for gain.

- **Save audio settings** (`js/app.js:354-381`) calls the service's
  `setMakeupGain({dts, truehd, presetDts, presetThd, centerDts, centerThd})`
  (`service/service.js:1269-1321`), which clamps gain/centre and validates the
  preset against a fixed enum server-side — rejecting anything non-finite or
  unrecognised **before** it reaches a shell command — then writes both
  `gain.conf` files via `rootExec` (`w25GainConfWrite`, `service/service.js:259-265`):
  the bare-float gain line first (preserving the legacy format), then
  `drc=`/`drc_boost=`/`drc_cut=`/`center=` lines, written temp-file-then-`mv`
  so a decoder never reads a half-written value.
- On load (and Refresh), the panel calls `loadGain()` (`js/app.js:337-352`),
  which calls the service's `getMakeupGain()` (`service/service.js:1329-1354` —
  reads both files back, deriving the displayed preset name from the raw
  `(drc, drc_boost, drc_cut)` tuple since the config contract has no separate
  "preset" key) to populate all three controls per codec.
- New values take effect on the **next playback** — no re-detect, no reboot,
  no rebuild. Only available on the `webos25-armel-gst124` profile; refused
  cleanly elsewhere.
- For the full config-file format (including the four new keys), the
  preset table, how DRC interacts with make-up gain, and the by-ear tuning +
  release runbook (now gated by `src/test/run-tests.sh` before any
  cross-build), see
  [`../restore/TUNING-RUNBOOK.md`](../restore/TUNING-RUNBOOK.md).

#### A/B compare (hear the DRC on the same clip)

"I can't hear any difference between presets" is the failure mode this card is
built against — and it has already bitten once (the 2.4.0 read-back bug wrote
`drc=off` back over the user's selection, so the presets really were inert).
The **A/B compare** block at the bottom of the same card (`index.html:133-161`)
removes that class of doubt: one press renders the bundled DTS clip **twice**
on-device and reports a measured number, not an impression.

- **Render A/B** (`doAbRender()`, `js/app.js:410-450`) calls the service's
  `abPreview()` (`service/service.js:1362-1433`). It takes **no parameters** —
  variant **B** is read from the on-disk `gain.conf`, never from the caller —
  and produces:
  - **A** — `drc-mode=off drc-boost=100 drc-cut=100 makeup-gain-db=0.0
    center-boost-db=0.0` (the fully inert path);
  - **B** — the same clip with the user's **saved** gain / preset / dialogue
    boost.
- **`gain.conf` is never touched.** Both variants are expressed as `dtsdec`
  **GObject properties** on the `gst-launch` command line (`w25AbProps()`,
  `service/service.js:820-830`), which override the config file for that one
  process. The script md5s **both** `gain.conf` files before the first render
  and after the last one and returns the hashes, so "unchanged" is proof rather
  than a claim — the UI shows them.
- **Measured delta.** There is no ffmpeg on the TV, so a second pass per variant
  runs GStreamer's `level` element and the RMS/peak values are parsed out of the
  `gst-launch-1.0 -m` bus messages (`AB_LEVEL_AWK`). On the C5 with
  `DTS-in-mp4.mp4` the ground truth is `drc=off` → mean −44.6 dB / peak −14.9 dB
  vs `drc-mode=rf` → −40.9 / −14.5, i.e. **+3.7 dB mean, +0.4 dB peak**. If
  `level` is not registered on the TV the numbers are reported as *not measured*
  with the reason — never invented.
- **Where the renders go, and why.** `$APPBASE/payload/testfiles/` — the app's
  own install directory, beside the bundled samples. The existing play-by-ear
  buttons already load `payload/testfiles/DTS-in-mp4.mp4` as a **relative URL**
  from `index.html` and that works on the device, so a file written into the
  same directory is reachable by the same mechanism; `/tmp` has no such evidence
  behind it and is very likely outside the app's document root. The directory is
  probed for writability first and the call fails with a clear message (never
  silently) if the install is read-only.
- **Verified on hardware (C5, webOS 25):** the in-app player *does* play the
  renders. They are written as **16-bit stereo PCM** (the most broadly supported
  WAV flavour, and ~6× smaller than the native 5.1/S32 output); both variants
  share the identical downmix so the A-vs-B difference is unaffected. If playback
  is ever refused the card degrades to **numbers only** — the measured delta is
  computed on-device and stays valid.
- **No `?r=` cache-buster on the player URL — ever.** webOS hands the `<audio>`
  `src` to `starfish-media-pipeline`, whose `filesrc` URI handler does **not**
  strip the query, so it opens a file literally named `…_a.wav?r=1` and errors
  with *Resource not found* (that was the "the in-app player refused the rendered
  clip" bug; reproduced directly with `gst-launch-1.0 playbin3 uri=…wav?r=1`).
  Freshness comes from the **filename** instead: every render gets a stamped
  basename (`AB_PREFIX + "a_" + <base36 stamp> + ".wav"`), so each take has a
  genuinely distinct, query-free URL.
- The renders are cleared by prefix (`dtsenabler_ab_*.wav`) at the start of every
  A/B and by `abCleanup()` when the app goes away, so at most one pair (~1.5 MB)
  is ever left on disk.
- **DTS only.** The bundled samples are DTS, so this exercises the DTS decoder
  path; it says so on the card and does not imply TrueHD coverage.

### `webos22-o22-gst118` — exact-firmware legacy override (EXPERIMENTAL; NOT HARDWARE VERIFIED)

This is an **app-only tester profile** in version 2.6.0. It does not change the
repo-root CX installer or the webOS-25 CLI. It reuses the four immutable legacy
files packaged in `payload/cx/`, but has its own state, boot hook, ownership
inspection, recovery path, and persisted baseline.

Detection selects the profile only when every identity and ABI field matches:

- hardware/OTA ID `HE_DTV_W22O_AFABATAA`;
- an OLED product ID containing `C2` or `G2`, plus a known board type;
- firmware `04.40.93` or `04.40.93.01`, webOS `7.4.0`;
- GStreamer `1.18.2`; and
- `/lib/ld-linux.so.3` with an ELF32 ARM EABI5 soft-float GStreamer userspace.

The read-only verdict then requires SHA-256 support and exact pristine hashes:

| Stock plugin | Required SHA-256 |
|---|---|
| `libgstlibav.so` | `6957fb676c11b3d6937b9c20cb8fb499167c233519b1881d03631c85fdedd2da` |
| `libgstisomp4.so` | `163007136c14e5373f8b47c6bef530a6730b61d68a28213bf01feccb6d5dbff7` |
| `libgstmatroska.so` | `83d2cd366abf264469406f4e5bc94d0f2544335c13ab9238ad7d6b9134ef4a18` |

`sha256sum` is preferred, with `busybox sha256sum` as the only fallback. An
unavailable tool, malformed digest, identity mismatch, other regional OTA ID,
other firmware, or stock hash mismatch is a hard refusal and cannot be forced.
Even an exact match reports **"firmware matched, hardware verification NO"** and
requires the same two-step **Try anyway (experimental)** UI flow; the service
accepts only the literal boolean `{force: true}` on the confirmed call.

- **Enable:** require all four payload files (`libgstlibav.so`,
  `libgstisomp4.so`, `libgstmatroska.so`, `libgstisomp4_1_8.so`), trace each
  through `/lib/ld-linux.so.3` before mutation, persist the target identity,
  stock hashes, `gstcool.conf`, registry path, and generated-file hashes, then
  install an authenticated regular-file guard hook. The mechanism binds libav,
  Matroska, and isomp4; `_1_8` is bound only if that stock target exists. It
  raises `avdec_dca` from rank 0 to 290 and builds an app-owned registry with
  `GST_REGISTRY_FORK=no`.
- **Ownership and recovery:** mutable state is isolated at
  `/var/lib/webosbrew/dtsenabler/c2`; the regular hook is
  `/var/lib/webosbrew/init.d/restore_dts_c2`. First enable refuses the legacy CX
  hook, any foreign C2 hook/state, or any managed target already mounted. Detect,
  boot, Disable, and Uninstall inspect complete mount sources/layers and the
  authenticated init/hook content. An interrupted transaction keeps coherent
  owner/baseline/recovery state for guarded teardown instead of guessing.
- **Disable/Uninstall:** owner-first routing remains available if the TV identity
  changes after installation. Only exact app-owned mounts and files are removed;
  foreign or stacked mounts are refused.
- **Capability boundary:** the package has Matroska and MP4 demux paths but no
  legacy MPEG-TS payload. C2/G2 therefore has MKV/MP4 DTS only, with no TS/M2TS,
  TrueHD/MLP, make-up gain, DRC, or A/B compare.
- **Self-test:** decodes only bundled `DTS-in-mp4.mp4` through `qtdemux !
  avdec_dca` using the persisted app-owned registry with registry updates and
  scanner forks disabled. A PASS proves userspace decode through the owned
  mechanism; it does **not** prove LG Media Player playback, sink/HAL output,
  reboot persistence, or audible hardware behavior.

The status panel exposes the OTA ID and firmware, exact verdict/reason, measured
hashes, owner/recovery state, and the permanent hardware-verification **NO** label.
B2/W22H, C3/W23O, and B3/W23H get diagnostic-only refusal profiles; C4/G4 and
other C2/G2 firmware remain unsupported/unknown.

### `cx-armv7-gst114` — demuxer-override (UNVERIFIED)

Mirrors the repo-root `install.sh` / `init_dts.sh`. CX-era firmware strips the
DTS pad from the demuxer, so the fix is library-override-centric.

- **Enable:** stage the rebuilt LG `.so` set → `/var/lib/webosbrew/dtsenabler/cx/gst/`;
  bake the GStreamer registry path from the exec session into `env.conf`; write
  `init_dts.sh`; install the boot hook `/var/lib/webosbrew/init.d/restore_dts`;
  apply now. The init script bind-mounts (read-only) the demuxer/libav libs over
  `/usr/lib/gstreamer-1.0/`, refreshes the registry, and bumps `avdec_dca`
  `0→290` with a `[downmix]` section in `gstcool.conf`.
- **Disable:** remove the boot hook, best-effort unmount all overrides +
  gstcool + registry bind. LG's own files were never modified.
- **Uninstall:** disable + `rm -rf /var/lib/webosbrew/dtsenabler/cx`.

---

## Security model

### Exec-bridge permissions (why the service can reach hbchannel)

hbchannel's `/exec` is already a **public** method that accepts any caller (once the
TV is rooted, hbchannel rewrites the device LS2 config to `allowedNames:["*"]` /
`inbound:["*"]`). The gate is on **our** side: a JS service's default role does not
grant **outbound** access, so its call to hbchannel is rejected. We therefore ship
three ACG manifest files next to `services.json` (discovered by naming convention;
appinstalld builds the `manifests.d` entry from them):

- `io.github.josippapez.dtsenabler.service.role.json` — `permissions[].outbound:["*"]`
  (the load-bearing line — lets us call `org.webosbrew.hbchannel.service`),
  `inbound:["*"]`, `allowedNames`.
- `io.github.josippapez.dtsenabler.service.api.json` — declares our own methods `public`.
- `io.github.josippapez.dtsenabler.service.perm.json` — client/outbound ACG grant.

**Guaranteed fallback (rooted TV):** if a given firmware doesn't auto-discover them,
copy the three files into the live LS2 dev config and restart the hub once:

```sh
cp io.github.josippapez.dtsenabler.service.role.json /var/luna-service2-dev/roles.d/
cp io.github.josippapez.dtsenabler.service.api.json  /var/luna-service2-dev/api-permissions.d/
cp io.github.josippapez.dtsenabler.service.perm.json /var/luna-service2-dev/client-permissions.d/
ls-control scan-services 2>/dev/null || killall -HUP ls-hubd 2>/dev/null || reboot
```

This is exactly the mechanism hbchannel uses for itself
(`webos-homebrew-channel/services/elevate-service.ts`). **Status: shipped in the
service dir; on-device auto-discovery vs. the fallback is still to be confirmed.**

### Security model

- **Everything handed to the exec service runs as root.**
- This app takes **no caller-controlled shell text**. Paths, filenames, ranks,
  and commands are author constants. `enable` accepts only a strict boolean
  `force`; audio settings accept bounded numbers and fixed preset names that are
  validated before fixed shell builders are selected.
- The **detected profile** is the only value that steers behaviour, and it is
  matched against a fixed allowlist (`PROFILE_W25`, `PROFILE_C2`, `PROFILE_CX`)
  before any mechanism runs. Diagnostic and unrecognised profiles are
  **refused**, never interpolated.
- Generated init scripts are written via `base64 -d` heredocs, so no content
  survives the write as shell syntax.
- The detection probe is strictly **read-only** — it inspects, it never mounts,
  copies, or modifies anything.

---

## Repository layout

```
webos25/app/
├── appinfo.json              # app metadata (id io.github.josippapez.dtsenabler)
├── icon.svg                  # placeholder launcher icon (convert to icon.png)
├── index.html                # status panel + Enable/Disable/Uninstall UI
├── css/style.css             # TV-remote styling + focus ring
├── js/app.js                 # controller: detect/status/enable/disable/uninstall, D-pad nav
├── service/
│   ├── package.json          # JS service manifest
│   ├── services.json         # Luna service + method registration
│   └── service.js            # detect + per-profile mechanism builders + exec
├── payload/
│   ├── webos25/              # <- drop libgstdtsdec.so + libdca.so.0 (see README)
│   │   ├── .gitkeep
│   │   └── README
│   └── cx/                   # <- shared immutable CX/C2 legacy .so set
│       ├── .gitkeep
│       └── README
├── .gitignore
└── README.md
```

---

## Build / package

Prereqs: the [webOS CLI](https://github.com/webosose/ares-cli)
(`npm i -g @webosose/ares-cli`), plus the vendored payload for each profile you
want to support.

```sh
# 1. Populate the payloads (see payload/*/README for provenance)
cp ../restore/out/libgstdtsdec.so ../restore/out/libdca.so.0   payload/webos25/
cp ../restore/truehd-out/libgstlibav.so ../restore/truehd-out/libav*.so* \
   ../restore/truehd-out/libsw*.so*                            payload/webos25-truehd/
cp ../restore/demux-out/libgstisomp4.so ../restore/demux-out/libgstmpegtsdemux.so \
                                                               payload/webos25-demux/
# small DTS samples for the self-test / play-by-ear (already bundled)
# payload/testfiles/{DTS-in-mp4.mp4,DTS-HD-MA-5.1.ts,DTS-HD-MA-5.1.m2ts}
cp -f ../../gst/*.so                                           payload/cx/  # shared CX + C2

# 2. Generate the icon
rsvg-convert -w 80 -h 80 icon.svg > icon.png

# 3. Package app + service into one .ipk (-s bundles the JS service dir)
ares-package . service
# -> io.github.josippapez.dtsenabler_<version>_all.ipk  (<version> = appinfo.json "version")
```

You can ship an `.ipk` with only one payload populated (e.g. webOS-25 only): the
service just refuses to enable on a profile whose payload is absent, logging a
clear WARN to `/tmp/dtsenabler.log`.

## Install

**Via Homebrew Channel — add our repository (works now, everyone):**
In the Homebrew Channel go to **Settings → Add repository** and paste:

```
https://josippapez.github.io/dts_restore_plus/api/apps.json
```

"DTS Enabler" then appears in the app list and installs — the `.ipk` is pulled from
the GitHub release and its **sha256 is verified** against the hosted manifest
(`https://github.com/josippapez/dts_restore_plus/releases/latest/download/io.github.josippapez.dtsenabler.manifest.json`).
Updates flow automatically as new releases are tagged (the repo is regenerated by CI).

*(Or, once accepted into the official [`webosbrew/apps-repo`](https://github.com/webosbrew/apps-repo), it appears in HBC by default with no custom repo — see `packaging/homebrew/`.)*

**Sideload (development), TV in dev mode + rooted:**

```sh
ares-setup-device
ares-install ./io.github.josippapez.dtsenabler_*_all.ipk   # glob matches whatever version was packaged
ares-launch io.github.josippapez.dtsenabler
```

The Homebrew Channel must be installed on the TV — the app calls its exec
service for all root work.

---

## STATUS (honest)

Current app and service version: **2.6.0**.

| Profile | TV family | Mechanism | Status |
|---|---|---|---|
| `webos25-armel-gst124` | LG C5 / G5 (webOS 25, GStreamer 1.24, armel soft-float) | decoder-inject (patched dtsdec + libdca) + TrueHD (avdec_truehd) | **Mechanism VERIFIED playing on a real C5** (via the `restore/` CLI install): both DTS and TrueHD decode and play, LG's sink receives `audio/x-raw, S32LE` (5.1 for DTS, and 8ch/7.1 for a TrueHD Atmos title — its full base bed), persistent across reboot. NOTE: the exec-bridge **role/permission manifest is now shipped** (service `*.role.json` with `outbound:["*"]` + api/perm files — see "Exec-bridge permissions"), so the app's detect/enable/test should reach the Homebrew Channel; **on-device confirmation (auto-discovery vs. the `/var/luna-service2-dev/` fallback) is pending**. The `restore/install.sh` CLI path remains the verified route. The app now also stages the **container demuxers** and includes a **self-test + play-by-ear** for mp4/ts/m2ts. |
| `webos22-o22-gst118` | Exact global OLED C2/G2 `W22O`, firmware `04.40.93`/`04.40.93.01`, webOS `7.4.0`, GStreamer `1.18.2`, soft-float | exact-gated legacy libav/Matroska/isomp4 override + `avdec_dca` rank | **EXPERIMENTAL; hardware verification NO.** Requires exact identity plus the three stock SHA-256 values and a two-step opt-in. Firmware inspection, QEMU plugin load/decode, and community reports support a tester path, but no rooted C2/G2 ran Enable, playback, reboot, Disable, or Uninstall. MP4 self-test only; MKV/MP4 capability, no TS/M2TS/TrueHD/gain/A-B. |
| `cx-armv7-gst114` | OLED CX / BX / C1 / NanoCell (webOS 3–6, GStreamer 1.14) | demuxer-override (rebuilt LG libs + `avdec_dca` rank) | **Carried over, UNVERIFIED by this project** — no CX hardware. The payload itself is measured ELF32 ARM EABI5 soft-float; the stock CX loader/e_flags were not captured on-device. Confirm the target with `detect` before trusting field compatibility. |
| C3/G3/M3 (`W23O`) | webOS 23, GStreamer 1.18.5 | **none needed** | **Native DTS — LG restored it in 2023.** `dts_audiodec` is registered and `gstcool.conf` raises it to rank 290. Only local **MKV** is gated (`enable-dts` defaults false). Owner-reported working via upstream `dts_restore`. The current `webos23-w23o-diagnostic` refusal is **wrong and pending removal** — see [`../docs/FIRMWARE-COMPATIBILITY.md`](../docs/FIRMWARE-COMPATIBILITY.md). |
| C4/G4/M4/T4 (`W24G`, `W24O`) | webOS 24 | **none needed** | **Native DTS, local decode *and* passthrough.** Nothing for this app to add; must not be offered a mechanism, and must not report "unknown". Product/press evidence, not extracted firmware. |
| B2/W22H, B3/W23H | webOS 22/23, GStreamer 1.18.2/1.18.5 | diagnostic only | **Explicit non-forceable refusal profiles.** B2 has no registered decoder and an unverified Realtek sink path; B3 has a distinct proprietary decoder/sink path. |
| C1/G1 (2021) | webOS 6 | none | **Decoder genuinely absent, never analyzed.** The real remaining gap; no firmware extraction or on-device evidence exists. |
| Other C2/G2 firmware; anything else | — | none | Exact C2 mismatches receive a C2 diagnostic refusal; unrecognized targets remain unsupported/unknown. |
| anything else | — | none | Detector emits `unknown-*`; app refuses. |

Open questions: DTS decodes to **S32LE, up to 5.1**, and TrueHD to **S32LE** —
LG's integer-only sink accepts these (confirmed on-device; the earlier F32LE issue
was fixed by converting dtsdec's output to S32LE). The remaining unknown is whether
the TV **renders full surround** to speakers/eARC or downmixes to stereo (not
independently measured). Bitstream **passthrough** to an AVR is out of scope
(months of proprietary-lib RE). See `../docs/MULTI-MODEL.md` for the full gap list.

## License

App code: LGPL-2.1-or-later. The vendored `.so` payloads (not committed here) are
**not uniformly LGPL** — `libgstisomp4.so`, `libgstmpegtsdemux.so`, `libgstlibav.so`
and the bundled `libav*`/`libsw*` are LGPL-2.1-or-later (ffmpeg is configured without
`--enable-gpl`), but `libgstdtsdec.so` links **libdca** and is therefore
**GPL-2.0-or-later**, as is the bundled `libdca.so.0`. So a distributed `.ipk`
contains GPL-2.0-or-later code; ship the corresponding-source offer covering it.
Full table in the [root README](../../README.md#license).
