# Multi-model DTS-restore: making the fix span LG webOS generations

Design document for generalizing `dts_restore` across LG TV families that differ on the three
axes that actually determine binary compatibility and fix strategy:

1. **CPU arch / float ABI** of the *GStreamer userspace* (not the kernel).
2. **GStreamer version** (drives plugin ABI and build system).
3. **How LG disabled DTS** (drives *which* fix mechanism to apply).

The C5 / webOS-25 branch is verified on hardware (see the epic). Everything about CX-class in
this repo is inherited from the shipping tool; everything about webOS 22/23/24 is unverified
(no hardware, no source drop) and is marked as such. This document distinguishes **[VERIFIED]**
(observed on-device or read directly from source/config) from **[ASSUMED]** (inference,
community report, or external knowledge).

Provenance of the evidence used here:
- `../README.md`, `../WEBOS25-DTS.md`, `../install.sh`, `../init_dts.sh` (the shipping CX tool).
- `.orchestration/dts-restore-webos25/EPIC.md` — the on-device C5 findings (target ABI + root
  cause + working solution).
- `.orchestration/dts-restore-improvement/issues/03-model-webos-support.md` — the
  model/webOS/GStreamer/rooting matrix research (cited below as *issue-03*).

---

## 1. Decision matrix (per target family)

| Target family | Arch / float ABI of GStreamer userspace | GStreamer | How LG disabled DTS | Required fix | Root available? |
|---|---|---|---|---|---|
| **CX-class** — webOS 3.x–6.x (CX/BX/C1/G1/C2/G2, 2020–2022 NanoCell/LCD) | **armv7, float ABI to VERIFY** (this repo's `.so`s are hard-float per WEBOS25-DTS.md; loader name not captured on-device) `[ASSUMED]` | **1.14.4** `[VERIFIED` for CX via `install.sh` gate; 1.14-class for the rest `ASSUMED` from community reports] | **Demuxer nerf** — DTS demux stripped from `libgstmatroska.so` (and mp4/ts variants) `[VERIFIED` by repo provenance notes] | **Override demuxer libs** (`libgstmatroska.so`, `libgstisomp4*.so`, optional `libgstmpegtsdemux.so`) rebuilt from LG source with DTS demux re-enabled, **+ raise `avdec_dca` rank** `0→290` in `gstcool.conf`, + registry refresh | RootMyTV v1/v2 (webOS 3.4–6.x), largely patched since 2022 `[ASSUMED` per issue-03] |
| **webOS 22 / 23 / 24** (C2/C3/C4/G-series 2022–2024) | Unknown — no source drop, no binary inspected `[ASSUMED` likely between 1.14 and 1.24] | Unknown | Unknown (no `lgstreamer` source repo; could be demuxer-nerf, decoder-absent, or re-tag) | **Undetermined** — cannot design a fix without a rooted unit to inspect | **No durable path.** RootMyTV excludes webOS 7(22)/8(23); DejaVuln: all webOS 9(24) release FW patched; faultmanager per-FW only `[ASSUMED` per issue-03] |
| **webOS 25 / C5-class** (C5/G5, chassis o22n3, "webOS 10") | **32-bit ARM, EABI5 soft-float** — `ld-linux.so.3`, e_flags `0x05000200`, triplet `arm-webos-linux-gnueabi`, glibc 2.35, glib 2.72, on an **aarch64 kernel** `[VERIFIED` on-device] | **1.24.0** `[VERIFIED` on-device + `gstreamer-webos-25/meson.build`] | **Re-tag + no decoder** — demuxer kept, but `matroskademux` emits `audio/x-unknown, codec-id=(string)A_DTS` (raw DTS bytes preserved), and **no** `dts_audiodec`/`avdec_dca`/`dtsdec` is shipped `[VERIFIED` on-device] | **Inject a patched `dtsdec`** (gst-plugins-bad 1.22, armel soft-float) whose sink caps are **widened to also accept `audio/x-unknown, codec-id=A_DTS`**, bundle `libdca.so.0`, then rank it and inject into the media registry — **no LG library is overridden** `[VERIFIED` to autoplug on-device] | **faultmanager only**, factory FW pre-10.1 OTA — narrow window `[ASSUMED` per issue-03] |

### Notes on the two DTS-disable mechanisms (they are genuinely different fixes)

- **CX (demuxer nerf).** The demuxer never emits a usable DTS pad, so the decoder rank is
  irrelevant until you swap in a demuxer that *does* demux DTS. The fix is therefore
  **library-override-centric** (bind-mount rebuilt LG demuxer `.so`s over the nerfed ones) plus a
  one-line `gstcool.conf` rank bump so the already-present `avdec_dca` autoplugs. This is exactly
  what `../init_dts.sh` does (steps 1–3).
- **webOS 25 (re-tag + decoder-absent).** The demuxer is fine structurally but re-labels the DTS
  track as `audio/x-unknown` and there is **no decoder at all**. Overriding LG libraries is both
  unnecessary and dangerous (ABI is 1.24, not 1.14). The fix is **decoder-injection-centric**: add
  a *new* plugin whose caps deliberately match LG's `audio/x-unknown, codec-id=A_DTS` so
  `decodebin`/`decproxy` autoplugs it, and get it into the registry the media process trusts.

  > **Reconciling the two internal docs:** `../WEBOS25-DTS.md` (written earlier) assumed the
  > demuxer would emit `audio/x-dts` and that a normally-capped `dtsdec`/`avdec_dca` ranked in
  > `gstcool.conf` would autoplug. The **on-device EPIC finding supersedes this**: the demuxer
  > emits `audio/x-unknown, codec-id=A_DTS`, which is why the *widened-caps* patch to `dtsdec` is
  > required — a stock-capped decoder would never be selected. Treat the EPIC as authoritative and
  > WEBOS25-DTS.md's rank-only recipe as obsolete for the C5. This is consistent with issue-03's
  > source read: `DTS_SUPPORT`/`dca` compiled off makes `matroska-demux.c` return NULL for the DTS
  > codec-id, collapsing the caps to unknown.

---

## 2. Per-target build recipe summary

### 2.1 CX-class — `cx-armv7-gst114`

- **Build what:** demuxer plugins from LG's released 1.14.4 source (per `../README.md` provenance):
  `libgstmatroska.so`, `libgstisomp4.so`, `libgstisomp4_1_8.so` from `lgstreamer/gst-plugins-good@lg`
  (DTS demux re-enabled + MKV Dolby Vision); `libgstlibav.so` from `lgstreamer/gst-libav@lg`
  (dca decode + forced stereo-integer downmix + `[downmix]` coefficients); optional
  `libgstmpegtsdemux.so` from `lgstreamer/gst-plugins-bad@lg` (m2ts/BD).
- **Toolchain:** WebOSBrew SDK — `meta-lg-webos-ndk` + `starfish-sdk-x86_64`, triplet
  **`arm-webos-linux-gnueabi`**, targeting CX-era **armv7a-neon**. Build system: **autotools**
  (`git checkout tags/1.14.0 -b lg && ./configure --disable-gtk-doc …`) `[ASSUMED` per issue-03].
  - **Float-ABI verification still owed:** WEBOS25-DTS.md calls the CX libs "armv7 hard-float," but
    the loader name / e_flags of a CX system `.so` were never captured on-device. The C5 turned out
    to be *soft*-float despite sharing the `arm-webos-linux-gnueabi` triplet name — so CX's float ABI
    must be read off a real CX (`detect-target.sh` does exactly this) before trusting "hard-float."
- **Select at install time:** matched when the detector reports GStreamer `1.14.x` and a
  `cx-*-gst114` profile; the already-present `avdec_dca` means no decoder needs shipping.
- **These are the binaries already in `../gst/`.** No new work unless supporting a chassis whose
  `gstcool.conf` schema differs.

### 2.2 webOS 25 / C5-class — `webos25-armel-gst124`

- **Build what:** a **single patched `dtsdec`** plugin (`libgstdtsdec.so`) from **gst-plugins-bad
  1.22** source (ABI-stable against the TV's 1.24 loader — verified on-device), with the sink caps
  widened to also accept `audio/x-unknown, codec-id=A_DTS`. Bundle **`libdca.so.0`** (armel)
  alongside it; bake `RPATH` to the payload libs dir. `[VERIFIED` build + on-device load]
- **Toolchain:** **Debian armel cross** via Docker — `arm-linux-gnueabi-gcc` (soft-float EABI5),
  `-DHAVE_ORC=0`. Constraint that made it work: **max referenced GLIBC symbol ≤ 2.4** (TV has
  glibc 2.35, so any ≤ 2.35 is safe; the build stays well under). All other deps present on the TV
  except `libdca`, which is bundled. `[VERIFIED` per EPIC]
  - **Not the meson `gstreamer-webos-25` route.** WEBOS25-DTS.md proposed building from
    `lgstreamer/gstreamer-webos-25` (Meson/Ninja, 1.24) with an aarch64 cross. That was based on the
    wrong ABI assumption (aarch64) and the wrong caps assumption. The **working** path is the Debian
    armel 1.22 cross above. Keep the meson tree only as a reference for the LG-side `dca`/`DTS_SUPPORT`
    guard, not as the build basis.
- **Select at install time:** matched when the detector reports GStreamer `1.24.x`, loader
  `ld-linux.so.3`, soft-float e_flags `0x05000200`, and **no** `dtsdec`/`avdec_dca` present.

### 2.3 webOS 22/23/24 — no recipe yet

Deliberately none. Without a rooted unit to run `detect-target.sh` (arch/float, GStreamer version)
and to inspect whether DTS is demuxer-nerfed vs re-tagged vs decoder-absent, any recipe would be a
guess. The detector will emit an `unknown-*` profile for these so the installer refuses rather than
mis-applies a CX or C5 mechanism.

---

## 3. Installer-architecture proposal

### 3.1 Where we are today

`../install.sh` ships **one** hardcoded library set in `../gst/` and a single mechanism
(`../init_dts.sh`: demuxer bind-mounts + registry refresh + `gstcool.conf` rank bump). Its platform
"gate" (lines 84–111) is an **allowlist that only suppresses a warning** — on any TV it applies the
*same* CX 1.14.4 demuxer-override mechanism, which on a webOS-25 TV would bind-mount 1.14/armv7
libraries over 1.24/armel ones and **break** MKV/MP4 playback (WEBOS25-DTS.md §1). So today's
installer is not just "unaware" of other targets — on webOS 25 it is actively harmful.

### 3.2 Proposed structure — profile-driven single installer

```
dts_restore/
  install.sh                 # thin front-end: detect -> select profile -> dispatch
  webos25/
    detect-target.sh         # this deliverable; prints a machine-readable profile
    MULTI-MODEL.md           # this document
  payload/
    cx-armv7-gst114/
      manifest.conf          # mechanism=demuxer-override; libs, ranks, gstcool key
      gst/                    # the existing ../gst/*.so demuxer set
      apply.sh               # == today's init_dts.sh (bind-mounts + rank + registry)
    webos25-armel-gst124/
      manifest.conf          # mechanism=decoder-inject; plugin, bundled libs, caps note
      libs/                  # libgstdtsdec.so (widened caps) + libdca.so.0 (armel)
      apply.sh               # inject dtsdec into registry, rank, bind-mount registry
```

### 3.3 Flow

1. `install.sh` runs `webos25/detect-target.sh`, which prints `PROFILE=<name>` plus the raw probes.
2. It maps `PROFILE` to a `payload/<PROFILE>/` directory.
   - `cx-*-gst114` → the demuxer-override mechanism (backward-compatible with today's behavior).
   - `webos25-armel-gst124` → the decoder-inject mechanism.
   - `unknown-*` → refuse by default; `--force <profile>` to override (mirrors today's `-y`, but the
     operator must name the profile, so we never silently apply CX libs to a non-CX ABI).
3. It copies `payload/<PROFILE>/` to a per-profile dir under `/var/lib/webosbrew/dts_restore/<PROFILE>/`
   and installs a boot hook that runs that profile's `apply.sh` (idempotent, always `exit 0` — same
   failsafe discipline as `../init_dts.sh` line 74).
4. Each `apply.sh` reads `manifest.conf` so the mechanism is data-driven, not hardcoded per TV.

### 3.4 Reconciliation with the current CX `install.sh`

- Keep `install.sh`'s robust script-path discovery, root check, media-player-running guard, and
  registry-env capture (`GST_REGISTRY_1_0`) — all still needed.
- Replace the single-`gst/` copy + warn-and-proceed allowlist with the detect→select→dispatch flow.
- The CX profile's `apply.sh` **is** today's `init_dts.sh` verbatim, so existing CX/BX/C1/C2/NanoCell
  installs behave identically; only the selection layer changes.
- The off-target warning stays, but escalates from "warn, then apply CX libs anyway" to "refuse an
  ABI-mismatched mechanism unless an explicit `--force <profile>` names it."

---

## 4. Honest gaps

- **Unverified targets (no hardware):** webOS 22/23/24 entirely — arch, float ABI, GStreamer
  version, and DTS-disable mechanism are all unknown; no `lgstreamer` source drop exists for them.
  BX/B9/C9 CX-library reuse is a community *request*, not a confirmed report (issue-03 §2). C1/C2/
  NanoCell "working" rests on a single community report, not repo/binary verification.
- **CX float-ABI verification still owed:** the "armv7 hard-float" claim for CX is repo lore, not
  an on-device measurement. The C5 proved the `arm-webos-linux-gnueabi` triplet can be *soft*-float,
  so CX's loader name + e_flags must be read on a real CX before the `cx-armv7-gst114` profile can
  assert hard-float. `detect-target.sh` captures exactly these.
- **Surround-at-output open question (C5):** RESOLVED that LG's sink is **integer-only** — `dtsdec`
  now emits **S32LE** (up to 5.1), which the sink accepts (the earlier F32LE issue is fixed). The
  remaining unknown is whether the TV **renders full surround** to speakers/eARC or downmixes to
  stereo (not independently measured). Bitstream passthrough is out of scope; the `experimental/`
  LPCM-converter route would only be needed if the sink turns out to downmix.
- **Real-playback proof on C5: DONE** — both DTS and TrueHD confirmed playing on-device through LG's
  `starfish-media-pipeline`/`decproxy` (decproxy autoplugs the injected decoders; the sink receives
  S32LE), not just `decodebin`. Persistent across reboot.
- **Root-availability limits (webOS 22+):** even a perfect build is moot without root. RootMyTV
  excludes webOS 7(22)/8(23); webOS 9(24) release firmware is patched; webOS 25 is faultmanager-only
  on pre-10.1 factory firmware. Root, not the build, is the harder gate above CX-era (issue-03 §4).
- **Chassis-codename mapping** (o22n3/o24n/k24n ↔ webOS-25 family) is external web knowledge, not
  verified in the source tree (issue-03 §2).

---

*Status: design + detection skeleton. Read-only research; no TV was contacted and nothing was
committed. Build scripts, `payload/<profile>/` population, and the refactored `install.sh` dispatcher
are follow-on work gated on the verifications above.*
