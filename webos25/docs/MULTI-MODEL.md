# Multi-model DTS-restore: making the fix span LG webOS generations

Design document for generalizing `dts_restore` across LG TV families that differ on the three
axes that actually determine binary compatibility and fix strategy:

1. **CPU arch / float ABI** of the *GStreamer userspace* (not the kernel).
2. **GStreamer version** (drives plugin ABI and build system).
3. **How LG disabled DTS** (drives *which* fix mechanism to apply).

The C5 / webOS-25 branch is verified on hardware (see the epic). Everything about CX-class in
this repo is inherited from the shipping tool; the older webOS 22/23/24 generations are
firmware-documented for the C2/C3/B2/B3 sets but remain unverified on hardware and are marked
as such. This document distinguishes **[VERIFIED]**
(observed on-device), **[FIRMWARE]** (read from an extracted firmware image but not run on that
target), and **[ASSUMED]** (inference, community report, or external knowledge). See
[`FIRMWARE-COMPATIBILITY.md`](FIRMWARE-COMPATIBILITY.md) for the dated firmware analysis
(webOS 25 plus the older C2/C3/B2/B3 evidence)
and why firmware evidence must not be presented as hardware verification.

Provenance of the evidence used here:
- `../../README.md`, `WEBOS25-DTS.md`, `../../install.sh`, `../../init_dts.sh` (the
  shipping CX tool plus the webOS-25 background document).
- `.orchestration/dts-restore-webos25/EPIC.md` — the on-device C5 findings (target ABI + root
  cause + working solution).
- `.orchestration/dts-restore-improvement/issues/03-model-webos-support.md` — the
  model/webOS/GStreamer/rooting matrix research (cited below as *issue-03*).

---

## 1. Decision matrix (per target family)

| Target family | Arch / float ABI of GStreamer userspace | GStreamer | How LG disabled DTS | Required fix | Root available? |
|---|---|---|---|---|---|
| **CX-class** — webOS 3.x–6.x (CX/BX/C1/G1, 2020–2021 NanoCell/LCD) | Payload: **ELF32 ARM EABI5 soft-float** (measured); stock target loader/e_flags not captured on-device | **1.14.4** `[VERIFIED` for CX via `install.sh` gate; 1.14-class for the rest `ASSUMED` from community reports] | **Demuxer nerf** — DTS demux stripped from `libgstmatroska.so` (and mp4/ts variants) `[VERIFIED` by repo provenance notes] | **Override demuxer libs** (`libgstmatroska.so`, `libgstisomp4*.so`, optional `libgstmpegtsdemux.so`) rebuilt from LG source with DTS demux re-enabled, **+ raise `avdec_dca` rank** `0→290` in `gstcool.conf`, + registry refresh | RootMyTV v1/v2 (webOS 3.4–6.x), largely patched since 2022 `[ASSUMED` per issue-03] |
| **webOS 22 / C2-class** — LG/LX (`o22`, `W22O`; C2/G2, LX1Q/LX3Q, ART90) | **ARM EABI5 soft-float** `[FIRMWARE]` | stock **1.18.2** `[FIRMWARE]` (shipped legacy payload is 1.14.4) | **Re-tag + decoder absent** — Matroska emits `audio/x-unknown, codec-id=A_DTS`; `dts_audiodec` and `avdec_dca` are not registered despite stale rank/capability declarations `[FIRMWARE]` | DTS Enabler 2.6.0 implements `webos22-o22-gst118`: an app-only exact-identity/hash, two-step experimental opt-in that reuses the four legacy files with dedicated state. Firmware/QEMU evidence only; **not hardware-verified** | Root is still required; RootMyTV excludes webOS 7(22) `[ASSUMED` per issue-03] |
| **webOS 23 / C3-class** — LG/LX (`o22n`, `W23O`; C3/G3, M3) | **ARM EABI5 soft-float** `[FIRMWARE]` | **1.18.5** `[FIRMWARE]` | **Gated proprietary decoder** — `dts_audiodec` ships (rank 128; 290 via `gstcool.conf`) but demux is product-gated: Matroska `enable-dts` defaults false → `audio/x-unknown, codec-id=A_DTS`; MP4 retagged `audio/x-gst-fourcc-dtsc`; LG documents C3 as **DTS:X by-pass only** ([FIRMWARE] + product docs) | **No restore recipe.** Decoder presence + conditional demux are not verified local playback; no rooted unit confirmed the runtime gate | No durable path — RootMyTV excludes webOS 8(23) `[ASSUMED` per issue-03] |
| **webOS 22 / B2-class** — Realtek (`k8hp`, `W22H`; B2, QNED8x, UQ7x/9x) | **ARM EABI5 soft-float** `[FIRMWARE]` | **1.18.2** `[FIRMWARE]` | **No registered DTS decoder** — no `omxdtsdec1`/`avdec_dca`; demuxers re-tag (`audio/x-unknown, codec-id=A_DTS`, `audio/x-gst-fourcc-dtsc`); `rtkalsasink` statically accepts framed `audio/x-dts` but RTINGS reports **no DTS/DTS:X passthrough** ([FIRMWARE] + product docs) | **No automatic profile.** Legacy 1.14.4 payload decodes under QEMU, but sink/capability declarations are misleading and the closed S32LE→S16LE path is unverified | No durable path — RootMyTV excludes webOS 7(22) `[ASSUMED` per issue-03] |
| **webOS 23 / B3-class** — Realtek (`k8hpp`, `W23H`; B3, QNED8x, UR8x) | **ARM EABI5 soft-float** `[FIRMWARE]` | **1.18.5** `[FIRMWARE]` | Registers `omxdtsdec1` (configured rank 0) and supports product passthrough; capability JSON and `gstcool.conf` are byte-identical to B2, so those files are not sufficient discriminators `[FIRMWARE]` | **No restore recipe.** Local decode and the closed hardware path remain unverified even though the factory registers | No durable path — RootMyTV excludes webOS 8(23) `[ASSUMED` per issue-03] |
| **webOS 24 / C4-class** — LG/LX (`o22n2`, `W24G`; C4; also G4/M4/T4 `o24`/`W24O`) | Not extracted `[UNKNOWN]` | Not extracted `[UNKNOWN]` | Not extracted `[UNKNOWN]` | **None — no recipe.** Product-doc only `[UNKNOWN]` | No durable path — webOS 9(24) release FW patched `[ASSUMED` per issue-03] |
| **webOS 25 / C5** (`o22n3`, `W25G`, "webOS 10") | **32-bit ARM, EABI5 soft-float** — `ld-linux.so.3`, e_flags `0x05000200`, triplet `arm-webos-linux-gnueabi`, glibc 2.35, glib 2.72, on an **aarch64 kernel** `[VERIFIED` on-device] | **1.24.0** `[VERIFIED` on-device + firmware packages] | **Re-tag + no decoder** — `matroskademux` emits `audio/x-unknown, codec-id=(string)A_DTS` (raw DTS bytes preserved), and **no** `dts_audiodec`/`avdec_dca`/`dtsdec` is shipped `[VERIFIED` on-device] | **Inject patched `dtsdec` + `libdca`; fingerprint-gated overrides for TrueHD and MP4/TS DTS.** The current payload also shadows stock `libgstlibav.so`, `libgstisomp4.so`, and `libgstmpegtsdemux.so`; see `../README.md`. `[VERIFIED` on-device] | **faultmanager only**, factory FW pre-10.1 OTA — narrow window `[ASSUMED` per issue-03] |
| **webOS 25 / G5/M5** (`o24n`, `W25O`) | Same ARM EABI5 soft-float ABI as C5 `[FIRMWARE]` | **1.24.0** `[FIRMWARE]` | Extracted G5 firmware has byte-identical gated artifacts and common media-control binaries to the C5; runtime path not exercised `[FIRMWARE]` | **Strong candidate for the C5 payload, but hardware verification is still required.** Current hash-only logic would call it `verified`; the report recommends a separate firmware-match state. | Same webOS-25 rooting constraint `[ASSUMED]` |
| **webOS 25 / B5 and higher LCD/QNED** (`k24n`, `W25H`, Realtek) | Same ARM EABI5 soft-float ABI as C5 `[FIRMWARE]` | **1.24.0** `[FIRMWARE]` | Common `decproxy`/`umediaserver` stack and MP4/TS demuxers match, but stock libav, configs, and the `rtkalsasink`/`omxlpcmdec` path differ `[FIRMWARE]` | **Experimental opt-in only pending a real B5/W25H test.** Static ABI compatibility is not sink/HAL proof. | Same webOS-25 rooting constraint `[ASSUMED]` |
| **webOS 25 / lower LCD/QNED/NanoCell** (`k25lp`, `W25P`, Realtek) | Firmware metadata only; rootfs not inspected `[ASSUMED]` | webOS 10.3.1 images exist `[FIRMWARE]` | Unknown | **No automatic compatibility claim.** Extract separately; do not inherit the `k24n` result. | Same webOS-25 rooting constraint `[ASSUMED]` |

### Notes on the two DTS-disable mechanisms (they are genuinely different fixes)

- **CX (demuxer nerf).** The demuxer never emits a usable DTS pad, so the decoder rank is
  irrelevant until you swap in a demuxer that *does* demux DTS. The fix is therefore
  **library-override-centric** (bind-mount rebuilt LG demuxer `.so`s over the nerfed ones) plus a
  one-line `gstcool.conf` rank bump so the already-present `avdec_dca` autoplugs. This is exactly
  what `../../init_dts.sh` does (steps 1–3).
- **C2 exact experimental profile (re-tag + decoder absent).** The app reuses the four legacy
  demux/libav files only for the exact analyzed global firmware, after matching identity, ABI,
  GStreamer 1.18.2, and three stock SHA-256 values. It is not a rebuilt 1.18 payload and is not a
  general C2 claim: all other C2/G2 variants are refused. Its dedicated owner/baseline/hook and
  MP4 userspace self-test keep this firmware recruitment path separate from CX mutable state and
  from hardware verification.
- **webOS 25 (re-tag + decoder-absent).** For the original MKV DTS problem, the demuxer is fine
  structurally but re-labels the DTS track as `audio/x-unknown` and there is **no decoder at all**.
  That fix is **decoder-injection-centric**: add a *new* plugin whose caps deliberately match LG's
  `audio/x-unknown, codec-id=A_DTS` so `decodebin`/`decproxy` autoplugs it, and get it into the
  registry the media process trusts. The current, broader payload also uses fingerprint-gated
  same-ABI overrides for TrueHD and MP4/TS DTS. Arbitrarily applying the old 1.14 LG libraries is
  still dangerous; the webOS-25 overrides are separate 1.24 builds guarded by stock fingerprints.

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

- **Build what:** demuxer plugins from LG's released 1.14.4 source (per `../../README.md` provenance):
  `libgstmatroska.so`, `libgstisomp4.so`, `libgstisomp4_1_8.so` from `lgstreamer/gst-plugins-good@lg`
  (DTS demux re-enabled + MKV Dolby Vision); `libgstlibav.so` from `lgstreamer/gst-libav@lg`
  (dca decode + forced stereo-integer downmix + `[downmix]` coefficients); optional
  `libgstmpegtsdemux.so` from `lgstreamer/gst-plugins-bad@lg` (m2ts/BD).
- **Toolchain:** WebOSBrew SDK — `meta-lg-webos-ndk` + `starfish-sdk-x86_64`, triplet
  **`arm-webos-linux-gnueabi`**, targeting CX-era **armv7a-neon**. Build system: **autotools**
  (`git checkout tags/1.14.0 -b lg && ./configure --disable-gtk-doc …`) `[ASSUMED` per issue-03].
  - **Target verification still owed:** the payload itself is measured ELF32 ARM EABI5 soft-float,
    but the loader/e_flags of a stock CX system `.so` were never captured by this project. Read them
    from a real target (`detect-target.sh` does exactly this) instead of inferring from the triplet.
- **Select at install time:** matched when the detector reports GStreamer `1.14.x` and a
  `cx-*-gst114` profile; the already-present `avdec_dca` means no decoder needs shipping.
- **These are the binaries already in `../../gst/`.** No new work unless supporting a chassis whose
  `gstcool.conf` schema differs.

### 2.2 webOS 25 / C5 baseline — `webos25-armel-gst124`

This subsection records the original DTS-only build recipe. The current shipping profile also
contains the separately built TrueHD/libav and MP4/TS demuxer payloads described in `../README.md`.

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

### 2.3 webOS 22 / exact C2/G2 — `webos22-o22-gst118`

This is a narrow app profile, not a new build and not a root-CLI profile.

- **Payload:** package the four tracked `../../gst/*.so` files under the app's shared immutable
  `payload/cx/`. All four must stage and resolve through `/lib/ld-linux.so.3`; the
  `libgstisomp4_1_8.so` bind is used only when that stock target exists. No legacy
  `libgstmpegtsdemux.so` is packaged.
- **Select only on the exact analyzed set:** global OTA ID `HE_DTV_W22O_AFABATAA`, OLED C2/G2
  product ID, known board, firmware `04.40.93` or `04.40.93.01`, webOS `7.4.0`, GStreamer
  `1.18.2`, `ld-linux.so.3`, soft-float, and these pristine stock SHA-256 values:
  `libgstlibav.so=6957fb676c11b3d6937b9c20cb8fb499167c233519b1881d03631c85fdedd2da`,
  `libgstisomp4.so=163007136c14e5373f8b47c6bef530a6730b61d68a28213bf01feccb6d5dbff7`, and
  `libgstmatroska.so=83d2cd366abf264469406f4e5bc94d0f2544335c13ab9238ad7d6b9134ef4a18`.
- **Apply:** after a two-step explicit opt-in, bind the legacy libav/Matroska/isomp4 files,
  raise `avdec_dca` to 290, and generate/bind an app-owned registry. C2 uses dedicated
  `/var/lib/webosbrew/dtsenabler/c2` state and `/var/lib/webosbrew/init.d/restore_dts_c2`, with
  exact mount-source ownership, authenticated generated scripts, baseline drift, and recovery
  checks. It never adopts the CX hook or state.
- **Capability/proof:** MKV/MP4 DTS, plus **optional** TS/M2TS when the bundled LG 1.14.4
  `libgstmpegtsdemux.so` stages and its dependencies resolve on the TV — it is skipped silently
  otherwise, so a TV without `libgstcodecparsers`/`libgstmpegts` keeps working MP4/MKV rather
  than failing the enable. TS/M2TS is **unverified on hardware**. The executable self-test is
  MP4-only. No TrueHD/MLP, gain, or A/B. A self-test PASS is userspace mechanism proof, not playback or
  hardware verification.

### 2.4 B2/B3 — diagnostic refusal only

Firmware evidence pins the arch/float ABI and stock GStreamer version for B2/B3
(Realtek 1.18.2/1.18.5), but no defensible local-playback recipe exists: no
registered decoder and an unverified Realtek sink path. The app emits
`webos22-w22h-diagnostic` or `webos23-w23h-diagnostic` with family-specific reasons
and never offers force. No family can inherit the C2 mechanism from a GStreamer
version match alone.

C3 and C4/G4 were also refused here originally. Both were misclassified — see §2.5.

### 2.5 C3/C4-era — native decoder present, so NOT a refusal

LG removed DTS for 2021–22, **restored it for 2023** (C3/G3/M3), kept it for **2024**
(C4/G4/M4/T4), then dropped it again for 2025. The first firmware pass read LG's
"DTS:X (by-pass only)" spec wording as absence of local decode and refused both
families. That was wrong, and the artifact evidence disagrees with it: on C3
`libgstlgaudiodec.so` registers `dts_audiodec` and `gstcool.conf` *raises* it to rank
290 — not something you do to a decoder meant to stay unreachable.

- **C4/G4/M4/T4** — native DTS, decode and passthrough. Nothing to add.
- **C3/G3/M3** — decoder healthy, but the stock demuxers are nerfed, so nothing ever
  emits `audio/x-dts` and the 290-ranked decoder is never reached. DTS is genuinely
  present and genuinely unusable. Owner-reported: upstream `lgstreamer/dts_restore`
  was needed on a real C3 to get DTS playing.

Detection is behavioural rather than a model list (`dts_audiodec` registered, plus
`avdec_dca`'s rank in `gstcool.conf`), so it covers regional variants and future sets
and cannot go stale like an OTA-ID allowlist — which matters because C4/G4 firmware
has never been extracted.

### 2.6 Why make-up gain / DRC is webOS-25 only, and should stay that way

This looks like a feature gap and is not one. **DRC exists because our software
decoders apply no loudness management, while LG's native decoders bake in
dialnorm/DRC.** It is implemented inside decoders *we build* — `gstdtsdec.c` for DTS
and the `mlpdec.c` patch in `build-truehd.sh` for TrueHD — so it can only exist where
we inject our own decoder.

That is exactly the set of generations where LG's decoder is gone:

| Generation | LG's DTS decoder | Correct approach | DRC needed? |
|---|---|---|---|
| C4/G4/M4/T4 | present, working | nothing | no |
| C3/G3/M3 | present, rank 290 | un-nerf the demuxer only | **no — LG's own applies** |
| C2/G2, C1/G1 | absent | inject a decoder | yes, would need porting |
| C5/G5 and later | removed | inject a decoder | yes — implemented |

So DRC is a **workaround for not having LG's decoder**, not a general feature. On a
generation that still has one, routing to it is strictly better than injecting ours:
you get LG's DSP decode *and* its loudness handling, with no payload, no ABI or
soft-float matching, and no version-skew risk.

Consequence for C3, and it improves on upstream: upstream's `init_dts.sh` raises
`avdec_dca` to 290, which is right for CX where no native DTS decoder exists. On C3
`dts_audiodec` is *already* at 290, so the better fix is to un-nerf the demuxer and
**leave the ranks alone** — LG's decoder autoplugs and handles loudness itself.
(Inference from the artifacts; the rank tie-break has not been measured, because no
C3 has been available.)

The app therefore refuses the gain/DRC/A-B endpoints off `webos25-armel-gst124`
(`service.js:2916, 3002, 3034, 3067`). **Do not "fix" that by widening it.** The only
generations that would legitimately need DRC are C2/G2 and C1/G1, and porting it
there means retargeting `build.sh`/`build-truehd.sh` from GStreamer 1.24 to 1.18.x
and shipping a second decoder payload — which the C2 profile's own design explicitly
rules out ("do not rebuild or introduce a GStreamer 1.18 payload"), and which no
available hardware could verify.

---

## 3. Historical root-CLI architecture proposal

This section records the earlier proposal for replacing the repo-root CX installer.
It has **not** been implemented: `../../install.sh` remains the inherited one-payload
CX CLI, while the profile dispatcher described above is implemented in the Homebrew app.

### 3.1 Where we are today

`../../install.sh` ships **one** hardcoded library set in `../../gst/` and a single mechanism
(`../../init_dts.sh`: demuxer bind-mounts + registry refresh + `gstcool.conf` rank bump). Its platform
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
      gst/                    # the existing ../../gst/*.so demuxer set
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
   failsafe discipline as `../../init_dts.sh` line 74).
4. Each `apply.sh` reads `manifest.conf` so the mechanism is data-driven, not hardcoded per TV.

### 3.4 Reconciliation with the current CX `install.sh`

- Keep `install.sh`'s robust script-path discovery, root check, media-player-running guard, and
  registry-env capture (`GST_REGISTRY_1_0`) — all still needed.
- Replace the single-`gst/` copy + warn-and-proceed allowlist with the detect→select→dispatch flow.
- The CX profile's `apply.sh` **is** today's `init_dts.sh` verbatim, so existing CX/BX/C1/NanoCell
  installs behave identically; only the selection layer changes.
- The off-target warning stays, but escalates from "warn, then apply CX libs anyway" to "refuse an
  ABI-mismatched mechanism unless an explicit `--force <profile>` names it."

### 3.5 Current app implementation

`../app/service/service.js` is the implemented profile dispatcher. It keeps webOS-25, CX, and C2
commands hardcoded rather than loading arbitrary manifests. The C2 profile is narrower than this
historical proposal: only its exact firmware/artifact match is forceable, the opt-in is a strict
boolean confirmed in two UI steps, and B2/C3/B3 diagnostic profiles can never force. C2 and CX
share only the immutable packaged source files; their state and hooks are separate.

---

## 4. Honest gaps

- **Unverified targets (no hardware):** arch/float/GStreamer version are firmware-documented
  for C2/C3 (1.18.2/1.18.5, ARM EABI5 soft-float) and B2/B3 (Realtek, 1.18.2/1.18.5), and
  the mechanisms are partially characterized (C2/B2: no registered DTS decoder; C3/B3: proprietary
  factories present, with product passthrough only established). C2 now has the exact-firmware
  app tester profile above, but no rooted C2/G2 has run it, so it remains explicitly experimental.
  B2/C3/B3 remain refused and C4/G4 remain unextracted.
  BX/B9/C9 CX-library reuse is a community *request*, not a confirmed report (issue-03 §2). C1/
  NanoCell "working" rests on a single community report, not repo/binary verification.
- **CX target ABI verification still owed:** the repository payload is measured ARM EABI5
  soft-float, but this project has not captured a stock CX loader path/e_flags on-device.
  `detect-target.sh` captures exactly these instead of inferring them from the toolchain triplet.
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
- **Machine/model mapping** (`o22`/`o22n`/`o22n2`/`o24`/`k8hp`/`k8hpp` and the webOS-25
  `o22n3`/`o24n`/`k24n`/`k25lp` with their OTA IDs) comes from the
  external TOH/caniroot data, not LG source in this tree. Firmware extraction confirms the OTA IDs
  embedded in the analyzed images but does not independently prove every marketing-model mapping.

---

*Status: historical CLI design plus current app behavior. The app ships the verified C5 mechanism
and the exact-firmware, hardware-unverified C2 tester profile; use `../app/README.md` for its current
contract and `FIRMWARE-COMPATIBILITY.md` for proof labels and extracted-firmware evidence.*
