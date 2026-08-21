# Firmware-derived compatibility evidence for LG webOS (webOS 22–25)

Investigation date: 2026-08-19

This report evaluates the firmware-analysis suggestion from
[`webosbrew/apps-repo#206`](https://github.com/webosbrew/apps-repo/pull/206#issuecomment-5337054774):
download LG firmware, extract its root filesystem with
[`epk2extract`](https://github.com/openlgtv/epk2extract), combine that with
[`dev-toolbox-cli`](https://github.com/webosbrew/dev-toolbox-cli) and the
[webOS Brew Table of Hardware](https://www.webosbrew.org/toh/), and use the result to
identify additional models worth testing.

## Verdict

The workflow is useful and worked across the analyzed webOS 22, 23, and 25 firmware. It can establish a
strong **firmware-only compatibility candidate**, but it cannot turn an untested TV
into a hardware-verified target.

- **Correction (2026-08-21): C3 and C4/G4 do not need this project at all.** LG
  removed DTS for 2021–22 (C1/G1, C2/G2), **restored it for 2023** (C3/G3/M3), kept
  it for **2024** (C4/G4/M4/T4 — full local decode *and* passthrough), then dropped
  it again for **2025** (C5/G5). The original pass read LG's "DTS:X (by-pass only)"
  spec wording as absence of local decode; that was wrong for both families. C4/G4
  need no profile; C3's only real gap is the MKV `enable-dts` demux gate. The
  remaining genuinely unsupported generations are **C1/G1 and C2/G2** (decoder
  absent) and **C5/G5 onward** (decoder removed).
- **C1/G1 is now analyzed (2026-08-21) and is the same case as C2/G2.** Both have an
  un-nerfed demuxer, an LG decoder that accepts `audio/x-dtsl` only, and no compiled
  ffmpeg `dca` decoder -- so both need a decoder INJECTED, and the shipped C2
  legacy-payload approach is the right shape. `avdec_dca=0` in their `gstcool.conf`
  looks like a one-line rank fix but is a leftover entry for an element that does not
  exist. Neither is verified on hardware.
- **G5/M5 (`o24n`, `W25O`) is the strongest next test target.** Its extracted
  firmware has the same ABI and byte-identical copies of all five analyzed stock
  artifacts (including the three plugin hashes used by the current gate). Its common media control
  path is also byte-identical to the C5. This is strong evidence that the payload is
  binary-compatible, but playback has not been run on a G5/M5.
- **B5 and the associated `k24n` sets (`W25H`) are plausible but materially
  different.** They use the same 32-bit ARM soft-float GStreamer 1.24 ABI and the
  same MP4/TS demuxer binaries, but have a different stock `libgstlibav.so`,
  different capability/rank configuration, and a Realtek-specific decoder/sink
  path. They must remain experimental until tested on-device.
- **`k25lp` (`W25P`) was mapped but not extracted in this pass.** It remains
  unknown, not equivalent to the analyzed B5 merely because both are Realtek
  families.
- **The analyzed global C2/G2 set now has a deliberately narrow app tester
  profile.** DTS Enabler 2.6.0 selects `webos22-o22-gst118` only after exact
  identity, firmware, ABI, GStreamer, and stock SHA-256 checks. It still requires
  a two-step experimental opt-in and remains **Firmware analyzed**, never
  hardware verified; regional variants and other firmware are refused.

The safe evidence labels are:

| Label | Meaning |
|---|---|
| **Hardware verified** | The payload was enabled and real media was played through LG's media pipeline on that target. |
| **Firmware match** | Extracted stock artifacts are byte-identical to a hardware-verified baseline, but the target itself was not tested. |
| **Firmware analyzed** | ABI, packages, dependencies, and configuration were inspected, but one or more relevant artifacts or paths differ. |
| **Unknown** | No extracted firmware and no on-device evidence. |

Only the first label should produce the user-facing verdict `verified`. The C2
profile deliberately reports "firmware matched, hardware verification NO" even
after its exact artifact gate and userspace self-test pass.

## webOS 25 target families

The generated model data in
[`@webosbrew/caniroot`](https://github.com/webosbrew/caniroot) separates webOS 25
into four machine/OTA families. Marketing year and webOS release are not sufficient
compatibility keys.

| Machine | OTA ID | Representative models | SoC family | Status here |
|---|---|---|---|---|
| `o22n3` | `HE_DTV_W25G_AFABATAA` | OLED C5, QNED9M | LG/LX | C5 hardware verified |
| `o24n` | `HE_DTV_W25O_AFABATAA` | OLED G5, M5 | LG/LX | G5 firmware match |
| `k24n` | `HE_DTV_W25H_AFADATAA` | OLED B5, higher QNED/LCD | Realtek | B5 firmware analyzed |
| `k25lp` | `HE_DTV_W25P_AFADATAA` | lower QNED, NanoCell, UA/LCD | Realtek | Not extracted |

There are separate Japanese OTA variants. Region/tuner variants should remain in
the manifest even when their rootfs artifacts later prove identical.

## Firmware inputs and integrity

LG's current `downloadFile` endpoint returned `403`/`NoAuthority` during this pass.
The images were therefore obtained through the independent
[EPK Firmware Archive](https://lg.slada.sk/), whose
[`processed_fw.json`](https://lg.slada.sk/processed_fw.json) records firmware
metadata and checksums. Every downloaded ZIP or direct EPK recorded below matched the
index checksum.

| Target | Firmware | ZIP SHA-256 | EPK SHA-256 |
|---|---|---|---|
| G5 / `W25O` | `33.31.69.01`, webOS `10.3.1-3007` | `45395d5a511d14425c5e7a50cca29e18e2bf3cf1cdfab0977d7127f37ed91efa` | `ff291f10454e782c8e0d5b227a7c8748a2acd6d0adefd8c5d7fbe2e2e11c8910` |
| B5 / `W25H` | `33.31.68.01`, webOS `10.3.1-3006` | `967723d63fea3d56fe00809245bb48efff9e5d06029e8c079d25ad8c56e2609b` | `f46cbc6f2de0a370853f4d350aefd5aa4b4cded3e5c057cbf9361a8e86555551` |

The comparison baseline was the real C5 on firmware `33.31.68`, webOS `10.3.1`.
Its pristine values came from `/var/lib/webosbrew/dts25/stock.fp`, not from the
currently bind-mounted live plugin paths.

Firmware images are not committed to this repository.

## Artifact results

### Files protected by the current gate

MD5 is shown because the shipping compatibility table and `stock.fp` use MD5 as an
identity key. SHA-256 should also be retained in any generated analysis manifest.

| Stock artifact | C5 `W25G` (hardware baseline) | G5 `W25O` | B5 `W25H` |
|---|---|---|---|
| `libgstlibav.so` | `0fd6d65ac9e3a78b393a615eaff8ac0b` | **same** | `73e01f3d1ed9d9feb95f1260bfdda2f9` |
| `libgstisomp4.so` | `57fe57060774f248c05af5a411fc9a8f` | **same** | **same** |
| `libgstmpegtsdemux.so` | `9b84a95cf29bc025553c7dee829b7cc1` | **same** | **same** |
| `device_codec_capability_config.json` | `b3a197a9e8d17f6bb8db7a2845b1d3cf` | **same** | `51321be9bc6639e51b9d348e1a2b4068` |
| `gstcool.conf` | `37d568cec3b7ace6e171a71a76210b42` | **same** | `e9b440e8162eb2352a9ef364ff9903e4` |

This has an immediate consequence for the current implementation. The verified-set
table in `app/service/service.js` and `restore/init_dts25.sh` keys only on GStreamer
major/minor and the three plugin MD5 values. The extracted G5 therefore matches the
C5 row and would currently be reported as `verified` with the C5 label, despite no
G5 hardware test having occurred.

That is a valid **binary-set match**, but it is not a valid **hardware-verification
claim**. These concepts should be represented separately.

> **Known unsafe label in the current implementation:** until the gate also checks an
> approved target family, its `verified` result means only "matches the C5 artifact
> set." On an untested G5/M5 it must be interpreted and presented as **Firmware
> match**, never as target hardware verification. This report does not promote G5/M5,
> and the existing experimental opt-in remains required for hardware recruitment.

### ABI and package evidence

Both extracted targets have:

- `/lib/ld-linux.so.3`;
- ELF32, little-endian ARM, EABI5;
- ELF flags `0x05000200` (`soft-float ABI`);
- ARMv7-A, Thumb-2, VFPv3, and NEON attributes;
- GStreamer `1.24.0-1240.27.ptl4tv.33`;
- glibc 2.35-era dependencies; and
- the same `DT_NEEDED` set for each inspected stock plugin.

The G5 package revisions identify LG builds (`soclg2`/`soclg3`). The B5 revisions
identify Realtek builds (`socrtk5`). For `libgstlibav.so` this results in a distinct
binary and build ID even though file size (145,352 bytes), soname, and dynamic
dependencies are the same. The current payload replaces that file and bundles its
own FFmpeg libraries, so this difference does not itself prove incompatibility; it
does prove that platform-specific builds exist and should not be ignored.

### Common control path and SoC-specific output path

The following G5 and B5 files are byte-identical to the C5 baseline:

- `/usr/lib/gstreamer-1.0/libgstdecproxy.so`;
- `/usr/lib/libgstcool-1.0.so.0.2400.0`; and
- `/usr/sbin/umediaserver`.

G5 additionally has the same `libgstlgaudiodec.so`, `gstcool.conf`, and codec
capability JSON as C5. Its resource-manager config differs only in O22/O24 video
resource names and additional O24 video resources; the audio resource quantities
are unchanged.

B5 crosses a real mechanism boundary:

- C5/G5 default to `audio=audiosink`, with `pcm_audiodec` and the LG audio decoder
  factories present in `gstcool.conf`.
- B5 defaults to `audio=rtkalsasink`, ranks `omxlpcmdec`, carries
  `libgstrtkalsa.so`/`libgstrtkaudio-1.0.so`, and has no
  `libgstlgaudiodec.so`.
- B5 still has the same common `decproxy`/`umediaserver` userspace and advertises
  DTS/DTSH/DTSE in the codec-capability JSON.

Static inspection can show that the injected software decoders fit the userspace
ABI. It cannot show that their multichannel PCM output is accepted and rendered by
the Realtek sink/HAL path.

## Older generations (webOS 22 / 23 / 24): C2 / C3 and the Realtek B2 / B3

Same machine/OTA keying and evidence labels as above. None of these targets has
been hardware-verified; the strongest claims here are `Firmware analyzed`.

| Machine | OTA ID | Representative models | SoC family | Status here |
|---|---|---|---|---|
| `o22` | `HE_DTV_W22O_AFABATAA` | OLED C2 / G2, LX1Q/LX3Q, ART90 | LG/LX | Firmware analyzed; exact experimental app profile for global C2/G2 firmware, hardware unverified |
| `o22n` | `HE_DTV_W23O_AFABATAA` | OLED C3 / G3, M3 | LG/LX | **Native DTS (LG restored it in 2023)**; only the MKV `enable-dts` gate blocks local MKV. Owner-reported working via upstream `dts_restore`. Do not refuse. |
| `o20n` | `HE_DTV_W21O_AFABATAA` | OLED C1 / G1 | LG/LX | **Extracted 2026-08-21.** Decoder absent (LG's takes `audio/x-dtsl` only; ffmpeg `dca` not compiled). Needs an injected decoder. GStreamer 1.16.2; never upgraded past webOS 6.5.3. |
| `o22` | `HE_DTV_W22O_AFABATAA` | OLED C2 / G2, LX1Q/LX3Q, ART90 | LG/LX | **Re-extracted 2026-08-21** (`04.40.90.01`, 7.4.0, GStreamer 1.18.2), confirming the shipped exact-gated experimental profile. Same shape as C1. |
| `o22n2` | `HE_DTV_W24G_AFABATAA` | OLED C4 | LG/LX | **Native DTS, decode + passthrough — no action needed.** Not extracted; product/press evidence. |
| `o24` | `HE_DTV_W24O_AFABATAA` | OLED G4 / M4 / T4 | LG/LX | **Native DTS, decode + passthrough — no action needed.** Not extracted; product/press evidence. |
| `k8hp` | `HE_DTV_W22H_AFADATAA` (analyzed global image `AFABATPU`) | OLED B2, QNED8x, UQ7x/9x | Realtek | Firmware analyzed (stock GStreamer 1.18.2) |
| `k8hpp` | `HE_DTV_W23H_AFADATAA` | OLED B3, QNED8x, UR8x | Realtek | Firmware analyzed (stock GStreamer 1.18.5) |

### Firmware inputs (older generations)

| Target | Firmware | ZIP SHA-256 | EPK SHA-256 |
|---|---|---|---|
| C2 / `W22O` | `04.40.93.01`, platform 7.4.0 (direct EPK) | — | `43fd884fe875374060ff1d03bb7d1afa0c0f7c85f5a673dc27c54f75bf053eca` |
| C3 / `W23O` | `03.33.25.01`, platform 8.3.1 | `b825682db22c2dcb03dd003427e1e4710ccdea105da7c3674e44e236056d5038` | `5d6961d048fcc5f18cdddcb6ec4f33d84de01207efba450f4a52076b7fee87f6` |
| B2 / `W22H` | `03.33.86.01`, platform 7.3.1 (direct EPK) | — | `d150d9cf77c334ce89b18e04ab5aed2f6b6e0c48df0f87db1d12df130fe0c527` |
| B3 / `W23H` | `03.31.82.01`, platform 8.3.1 | `173ff0c4fd1ab815c524650dfd31ba97962e897fb7e4e5abf008d14700fcd861` | `46a564bb1e1e0c824d77283871b4aa5f0b75edaf91af7ccee3e66e61b0a7a395` |

C2 and B3 were extracted and inspected, then their large temporary trees were removed
after checksums and results were recorded. Images are not committed to this repository.

### C2 (`o22` / `W22O`) — re-tagged input and no registered decoder

- Runtime: GStreamer `1.18.2-1182.webos4tv.12`, ELF32 ARM EABI5 soft-float.
- Decoder inventory: stock `libgstlgaudiodec.so` registers 12 factories but no
  `dts_audiodec`; stock `libgstlibav.so` has no `avdec_dca`. This is true even though
  `gstcool.conf` still contains `dts_audiodec=290`/`avdec_dca=0` and the capability
  JSON lists DTS/DTSH/DTSE.
- Container behavior: stock Matroska emits
  `audio/x-unknown, codec-id=(string)A_DTS`, so a normal DTS decoder cannot autoplug.
- Payload compatibility test: the repository's GStreamer 1.14.4 Matroska and libav
  plugins both load in the stock 1.18.2 rootfs under QEMU. The patched demuxer emits
  six-channel `audio/x-dts`, and `avdec_dca` decodes it to six-channel S32LE. This is
  userspace compatibility evidence, not an on-device playback test; the existing C2
  hardware status remains a community report.
- Product evidence: [RTINGS' C2 review](https://www.rtings.com/tv/reviews/lg/c2-oled)
  reports that the TV does not support DTS formats.

Selected artifact SHA-256: `libgstlgaudiodec.so`
`3be7e2fd06306740519d7d9672f38de04f5235cb79cb405aaf04b9ae7bf85a22`;
`libgstlibav.so` `6957fb676c11b3d6937b9c20cb8fb499167c233519b1881d03631c85fdedd2da`;
`libgstmatroska.so` `83d2cd366abf264469406f4e5bc94d0f2544335c13ab9238ad7d6b9134ef4a18`;
`libgstisomp4.so` `163007136c14e5373f8b47c6bef530a6730b61d68a28213bf01feccb6d5dbff7`;
`libgstmpegtsdemux.so` `5a0200daf8d1676b6ca4cf51b5cdfa68b417736a4e36aba8fc1f23486ba7b092`.

#### Implemented C2 app policy (version 2.6.0)

The Homebrew app, not either CLI installer, implements the resulting tester path.
Detection selects `webos22-o22-gst118` only when all of these are true:

- hardware/OTA ID is exactly `HE_DTV_W22O_AFABATAA`;
- the product ID identifies an OLED C2 or G2 and `board_type` is known;
- firmware is `04.40.93` or `04.40.93.01`, webOS is `7.4.0`;
- GStreamer is exactly `1.18.2`; and
- the loader is `/lib/ld-linux.so.3` and the inspected GStreamer userspace is
  ELF32 ARM EABI5 soft-float.

The verdict additionally requires a functioning SHA-256 tool and exact pristine
hashes for stock libav, isomp4, and Matroska (the three values immediately above).
`sha256sum` and `busybox sha256sum` are the only accepted tools. A Japanese or
other regional OTA ID, another firmware version, an invalid/unavailable digest,
or any artifact mismatch produces a non-forceable refusal. GStreamer 1.18.2 by
itself can never select or authorize the profile.

An exact match is still `unverified` until the user confirms the two-step
experimental opt-in. Enable stages the four tracked legacy files from root
`gst/`, checks their dynamic dependencies without executing plugin code, and
uses dedicated `/var/lib/webosbrew/dtsenabler/c2` state plus the authenticated
regular hook `/var/lib/webosbrew/init.d/restore_dts_c2`. Complete mount-source
ownership, persisted stock/config identity, and recovery markers protect boot,
refresh, Disable, and Uninstall; the app never adopts the CX hook or foreign
mounts/state.

The packaged legacy set restores the Matroska and MP4 paths and has no MPEG-TS
demuxer. The executable C2 self-test is therefore MP4-only and decodes through
the app-owned registry with `qtdemux ! avdec_dca`. A PASS upgrades none of the
hardware claims in this report: Media Player selection, sink/HAL output, audible
playback, reboot behavior, Disable, and Uninstall still require a rooted C2/G2.
B2 and B3 receive family-specific diagnostic refusals with no force option. **C3 and
C4/G4 were both misclassified in the original pass and are corrected below: they have
native DTS and must not be refused.**

### C3 (`o22n` / `W23O`) — native DTS, restored by LG; only the MKV demux gate remains

- Runtime: GStreamer `1.18.5-1185.webos4tv.8`, ELF32 ARM EABI5 soft-float.
- Decoder: `libgstlgaudiodec.so` registers `dts_audiodec` (rank 128, `gstcool.conf`
  raises it to 290) with sink caps `audio/x-dts`, `audio/x-private1-dts`,
  `audio/x-dtsx`, `audio/x-dtsh`, `audio/x-dtse`, `audio/x-dtsl`; source caps are
  interleaved raw PCM 1–384000 Hz, 1–8 channels.
- Demux is product-gated: stock Matroska emits `audio/x-unknown, codec-id=A_DTS`;
  the exposed `enable-dts` property defaults **false**, and setting it true emits
  `audio/x-dts, channels=6, rate=48000`. Stock MP4 emits
  `audio/x-gst-fourcc-dtsc`; `qtdemux` exposes no ordinary GObject DTS property.
- Player framework: `libpf-1.0.so.1.0.0` carries `platformSupportDTS`/`dts-support`
  and DTS caps/parser paths, but no text configuration value was found — app
  control is binary-evidence only, runtime value not proven.
- Product evidence: [LG documents C3](https://www.lg.com/us/tvs/lg-oled65c3pua-oled-4k-tv)
  as **DTS:X (by-pass only)** — but that page is unreliable here. LG **restored DTS in
  the 2023 line** after removing it for 2021–22, and the artifact evidence above
  agrees: `gstcool.conf` actively *raises* `dts_audiodec` to rank 290, which is not
  something you do to a decoder you intend to keep unreachable.

**Corrected verdict (supersedes the earlier diagnostic refusal).** C3/G3/M3 have
native DTS and must not be refused. The one real gap is the container gate: stock
Matroska emits `audio/x-unknown, codec-id=A_DTS` because `enable-dts` defaults
**false**, so MKV local playback stays blocked while passthrough and the other paths
work natively.

Owner report (2026-08-21): upstream `lgstreamer/dts_restore` was run on a real C3 and
DTS playback worked. That is direct hardware evidence against the refusal. The TV is
no longer available, so this cannot be re-probed; treat the mechanism as
owner-reported rather than measured by this project.

Best explanation consistent with all of it — LG restored DTS on C3, but left the
Matroska demux gate off, and overriding the demuxer is what unblocked MKV. Marked as
**inference**: no one has measured which containers were affected on that TV.

Note the gate is the same mechanism this project already flips for webOS 25. In LG's
own webOS-25 `matroska-demux.c` the property is literally `enable-dts` backing a
`dts_support` field defaulting FALSE (`:279`, `:363`, gated at `:8479-8489`) — the
same lineage, so no new mechanism would be needed for C3.

Selected artifact SHA-256: `libgstlgaudiodec.so`
`698d578f00a50164770c30f5d1058f90c5824bf720b87ea6b9486ae8e00e90e9`;
`libgstlibav.so` `99156a09fec83e869533f3e1359732fed8f83b4f70dcca44871fd5facadea2df`;
`libgstmatroska.so` `d07223a1cf35739690df9cc35f1a3303e3251b303085550816fbe67e8e879f6c`;
`libgstisomp4.so` `ddd4ee8af9181f74dc93c2bc5de9c053fd114bcf387f742ae153f56024ac023d`;
`libgstmpegtsdemux.so` `bd15f72db07b9d75b6446aa406e16e74af268c4fb1283ceed725a4c2e0810dac`.

### B2 (`k8hp` / `W22H`) — no decoder, no passthrough, misleading shared configs

- Runtime: GStreamer `1.18.2-1182.webos4tv.12-r0webos1starfish1`, ELF32 ARM EABI5,
  e_flags `0x05000200` soft-float.
- Decoder inventory: `libgstomx.so` registers 30 factories but **no DTS factory**;
  `gstomx.conf` has no `omxdtsdec1`; stock `libgstlibav.so` has no `avdec_dca`.
- Container behavior: stock Matroska has no `enable-dts` property and emits
  `audio/x-unknown, codec-id=A_DTS`; stock MP4 emits `audio/x-gst-fourcc-dtsc`;
  the DTS-HD MA TS sample exposes its video pad but no DTS audio pad.
- Output/passthrough boundary: `rtkalsasink` statically accepts framed
  `audio/x-dts`, but [RTINGS reports](https://www.rtings.com/tv/reviews/lg/b2-oled)
  the B2 supports **neither DTS nor DTS:X
  passthrough**. Sink caps do not prove enabled product behavior.
- Misleading shared declarations: B2's capability JSON lists DTS/DTSH/DTSE and is
  byte-identical to B3's (`7afdeb7bfeca7a9d4f48a0f2fa990fab43a3fa8e33e9e2af230871424c3cbbd4`);
  `gstcool.conf` is byte-identical to B3's (`38396936dc4bd39a5001f547828c599a317e888c8564d9b57c788f3b601c7290`).
  B3 nevertheless adds an `omxdtsdec1` factory and product passthrough, so these
  files are **not sufficient discriminators** within the Realtek family.
- Payload compatibility test: the repository's GStreamer 1.14.4 `libgstmatroska.so`
  loads under B2's 1.18.2 runtime and emits six-channel `audio/x-dts`; the repo
  `libgstlibav.so` then decodes it to six-channel S32LE under QEMU. That proves
  userspace plugin compatibility only — B2's Realtek sink advertises raw S16LE (not
  S32LE), so the closed decproxy/conversion/HAL path still requires hardware
  testing.

Selected artifact SHA-256: `libgstomx.so`
`4550d08009cbd194116ddb70ded0cb126cbb3f0f8d4d38bbe98e2043a8edb5cc`;
`libgstlibav.so` `9ccb788af60e99a470ac0fbb7929464f3147291a31089f7d2288a6c6d34a4bf4`;
`libgstmatroska.so` `3fde98059ddd1539ed0e9bdededbc90a8bb367916aedce67811d0c4ca0335660`;
`libgstisomp4.so` `8aceccc45b628104474cd912a19ead94d9bc174676b99927d6540ea669ab9dc9`;
`libgstmpegtsdemux.so` `7f0478198d41756260d770d709ea92e2a3c472ddd5da3582379e9a3966736e63`;
`libgstdecproxy.so` `767b183bd8a20e291e03ab1d757431c8d347ed0ae7deffdfb042b002ce4b35fd`;
`libgstrtkalsa.so` `bd2e602b0c5549625bd8ffc8e76e56a8ba793634ed36e32fe0ac292c26accd45`.

### B3 (`k8hpp` / `W23H`) — adds `omxdtsdec1` and product passthrough

- Runtime: GStreamer 1.18.5, ELF32 ARM EABI5 soft-float.
- Decoder inventory: `libgstomx.so` registers `omxdtsdec1`; its `gstomx.conf`
  entry uses `GstOMXDTSDec` with rank 0. The common `decproxy` remains ranked 300,
  so factory presence does not by itself establish the selected playback path.
- Output/passthrough boundary: `rtkalsasink` accepts compressed `audio/x-dts`, and
  [RTINGS reports](https://www.rtings.com/tv/reviews/lg/b3-oled) DTS passthrough.
  Neither source proves local file decoding through the closed hardware path.
- Capability JSON and `gstcool.conf` are byte-identical to B2 (hashes above), so
  those two shared files cannot discriminate product support.

Selected artifact SHA-256: `libgstomx.so`
`4fc5ccf8d29e4ebd0a4fe0cca437a6576de86249bc4e5e70c3c1bd43c9806862`;
`libgstlibav.so` `81cd482043943c43243326bccc155da00b60cb108b9e1e57a3f30ec4c34775c3`;
`libgstmatroska.so` `d07223a1cf35739690df9cc35f1a3303e3251b303085550816fbe67e8e879f6c`;
`libgstisomp4.so` `ddd4ee8af9181f74dc93c2bc5de9c053fd114bcf387f742ae153f56024ac023d`;
`libgstmpegtsdemux.so` `bd15f72db07b9d75b6446aa406e16e74af268c4fb1283ceed725a4c2e0810dac`;
`libgstrtkalsa.so` `a8e2f51a2c7278bab405083178624c373f3e6c1062d9da8919c3a8b96c6d4f38`.

### C4 / G4 / M4 / T4 (`o22n2` / `W24G`; `o24` / `W24O`) — native DTS, no action needed

webOS 24. Still not extracted, but the earlier "bypass-only" reading of LG's
[C4 specification](https://www.lg.com/us/tvs/lg-oled65c4pua-oled-4k-tv) was **wrong**
and is corrected here: the 2024 sets **retain full DTS support**, both local decoding
and passthrough. DTS:X inside DTS-HD MA decodes to the internal speakers and DTS
passes through over HDMI; only DTS:X IMAX Enhanced is absent.

**These models need no profile and no payload.** They must not be offered a
mechanism, and they must not be reported as "unknown" either — the correct verdict is
"your TV already decodes DTS natively; this app has nothing to add".

This sits in a clear generational pattern, which is the useful frame for the whole
project:

| Year | Models | Native DTS | Needs this project |
|---|---|---|---|
| 2020 | CX | removed by firmware update | yes — upstream `dts_restore`'s original target |
| 2021 | C1 / G1 | **absent** | yes — never analyzed; the real untouched gap |
| 2022 | C2 / G2 | **absent** | yes — the exact-firmware experimental profile |
| 2023 | C3 / G3 / M3 | **restored by LG** | no, except the MKV demux gate below |
| 2024 | C4 / G4 / M4 / T4 | **full support (decode + passthrough)** | **no** |
| 2025 | C5 / G5 and later | **dropped again** | yes — the main webOS-25 work |

Basis: LG restored DTS in the 2023 line after removing it for 2021–22, kept it for
2024, and dropped it again for 2025. Sources:
[HDTVTest](https://www.hdtvtest.co.uk/news/lg-quitely-drops-support-for-dts-sound-on-its-2025-t-vs),
[FlatpanelsHD](https://www.flatpanelshd.com/news.php?subaction=showfull&id=1743140114),
[HomeCineSolutions](https://en.homecinesolutions.fr/blog/posts/871-lg-drops-dts-support-on-its-2025-tvs-what-you-lose-and-what-stays-the-same).
This is product/press evidence, not firmware extraction — but it is corroborated by
the extracted C3 artifacts below, which show LG actively *raising* `dts_audiodec`'s
rank rather than removing it.

**Method note worth carrying forward:** "LG's spec page says bypass-only" turned out
to be unreliable for both C3 and C4. Marketing copy conflates passthrough with
decode. Prefer extracted artifacts, or an owner's on-device report, over the spec
sheet.

## C1/G1 (`o20n` / `W21O`) — extracted 2026-08-21; the blocker is decoder RANK only

The generation that had never been analyzed. Extracted from
`lib32-starfish-global-secured-o20n-koli-47-03.53.45_prodkey_usb_V3_SECURED.epk`
(latest LG build, 1.4 GB).

Identity, for a gate:

| field | value |
|---|---|
| machine / OTA | `o20n` / `HE_DTV_W21O_AFABATAA` |
| platform | `6.5.3` (archive shows the range `6.0.1` → `6.5.3`) |
| firmware | `03.53.45.01` |
| GStreamer | **1.16.2** (`libgstreamer-1.0.so.0.1602.0`) |
| kernel | `4.4.84-223` |
| starfish release | `Rockhopper release 6.0.1-4529 (kisscurl-kalaupapa)` (earliest build) |

**C1 never received the webOS 25 upgrade** — its newest build is still platform 6.5.3.
Unlike an upgraded C3 it therefore cannot fall through to `webos25-armel-gst124`; it is
genuinely isolated on GStreamer 1.16.2.

What is actually broken, layer by layer (all *read* from the extracted rootfs):

| layer | state |
|---|---|
| `libgstmatroska.so` | **not nerfed** — `A_DTS` present, `audio/x-dts` caps present, no `enable-dts` gate property |
| `libgstlxaudiodec.so` (LG's decoder, LX-generation name) | sink caps accept `audio/x-dtsl` **only** — no `audio/x-dts`, `x-dtsh`, `x-dtse`, `x-dtsx`. Retains `LXADEC_CODEC_DTS_CD` / `LXADEC_CODEC_DTS_EXPRESS` enums and the same vestigial `isDtsCoreless` / `isDTSSeamless` fields seen on the C5 |
| `/etc/gst/gstcool.conf` | `dts_audiodec=290` (line 30), **`avdec_dca=0`** (line 43) |

So the demuxer already emits `audio/x-dts`, and LG's own decoder will not accept it.
`gstcool.conf` also carries `avdec_dca=0` -- the exact string upstream
`lgstreamer/dts_restore`'s `init_dts.sh` rewrites to `290`.

**That looks like a one-line rank fix, and it is not.** Verified against the extracted
`libgstlibav.so`: ffmpeg's `dca` decoder is **not compiled in**, so there is no element
for a rank bump to enable. `avdec_dca=0` is a leftover config entry, not a lever.

Method and control, because the descriptor tables make this easy to get wrong:
`avcodec_descriptors` is compiled in regardless of which decoders are enabled, so
names like `DCA (DTS Coherent Acoustics)` and `DTS Express` prove nothing. A compiled
decoder is instead detectable by its own error strings. Positive control on the same
binary -- AAC internals are plainly present (`channel element %d.%d duplicate`,
`Expected to read %d SBR bytes actually read %d.`, `frame sync error`), so the
technique works on this file. Against that control, **no DCA internals appear at all**
(and none for MLP/TrueHD either).

C2 makes the same point more bluntly: its `libgstlibav.so` contains **no DCA string
whatsoever**, not even the descriptor. That matches the original pass's "no registered
decoder" reading for C2.

### C1 and C2 are the same case, and both need an injected decoder

| layer | C1 (`o20n`, GStreamer 1.16.2) | C2 (`o22`, GStreamer 1.18.2) |
|---|---|---|
| matroska demuxer | not nerfed (`A_DTS`, `audio/x-dts`, no gate) | not nerfed (identical) |
| LG's audio decoder | `libgstlxaudiodec.so`, `audio/x-dtsl` only | `libgstlgaudiodec.so`, `audio/x-dtsl` only |
| `gstcool.conf` | `dts_audiodec=290`, `avdec_dca=0` | `dts_audiodec=290`, `avdec_dca=0` |
| ffmpeg `dca` decoder | descriptor only, **not compiled** | **absent entirely** |

So neither is a rank-only target. Both need a **decoder injected**, which is the same
shape as CX and as the shipped `webos22-o22-gst118` profile -- so that profile's
legacy-payload approach is correct, not over-engineered. A C1 profile would mirror it:
exact identity gate, legacy payload, rank bump, dedicated state namespace.

Neither has been verified on hardware, and no C1 or C2 has been available.

Loudness consequence, unchanged: a legacy/stock `avdec_dca` applies no dialnorm or
DRC, so a fixed C1 or C2 would play quiet exactly as webOS 25 did before the
make-up-gain work. This project's DRC lives in decoders it builds, so giving either
one DRC means a patched decoder for GStreamer **1.16.2** (C1) or **1.18.2** (C2) --
two more build targets. See [`MULTI-MODEL.md` §2.6](MULTI-MODEL.md).

## Extraction gotcha that cost three false negatives (2026-08-21)

`epk2extract` reports `ERROR: Cannot decrypt EPK content (proper AES key is missing)`
in two completely different situations, and they look identical:

1. the AES key for that chipset/OS combo genuinely is not public, **or**
2. it never loaded any keys at all.

Tell them apart by the lines above the error. A real key miss prints one
`[+] Trying AES Key <hex>` line per candidate before failing. If you see
`Trying known AES keys...` followed immediately by the error and **no `[+]` lines**,
zero keys were loaded and the verdict is meaningless.

The cause is the key search path. `config_opts.config_dir` is set from the executable
location (`src/main.c:233`), and on macOS that resolves to **the directory containing
the binary itself**; `AES.key` is then read as `<config_dir>/AES.key`
(`src/util_crypto.c:26`) with no `keys/` subdirectory. So for a CMake build the keys
must be copied to `build/src/` next to the `epk2extract` binary. Putting them in
`keys/`, in `build/`, or in `build/src/keys/` all fail silently apart from a single
`Error: Cannot open key file.` printed *before* the banner, which is easy to dismiss
as noise about `MTK.key`.

This produced three false "no key available" conclusions in a row — for a C1
(`o20n`, webOS 6.5.3) and a C3 (`o22n`, webOS 25) — and briefly supported a wrong
conclusion that LG's current `_SECURED` prodkey images were undecryptable in general.
They are not: once the keys were placed correctly the C1 image extracted completely.

Related, and still true: the key is per **chipset/OS combo**, and for a combo with no
public key the only known recovery route is root access on that unit
([epk2extract#24](https://github.com/openlgtv/epk2extract/issues/24), maintainer
reply). So prefer the ORIGINAL firmware generation for a board over the newest one --
an old board running a new webOS is a fresh combo and may genuinely be unkeyed.

## Firmware archive

Images beyond what LG's support pages still offer come from
[`lg.slada.sk`](https://lg.slada.sk/processed_fw.json) (follow redirects; the index is
~3 MB of JSON). Its `fws` map is keyed by mirror path, and each entry carries
`zipFileSha256`, `zipFileSizeBytes` and a list of `epks` with `platformVersion`,
`firmwareversion`, `firmwareotaID`, `starfishRelease`, `kernelVersion` and
`buildTimestamp`. That is **header metadata only** -- no rootfs contents -- but it is
enough to pin a gate's identity fields without downloading anything, and it exposes
the full per-board upgrade path (the C3 alone has 26 distinct builds from platform
8.1.0 to 10.3.1). Files are fetchable directly with `curl -L`; a few entries have a
zero byte size and are simply absent (e.g. the C2's `04.40.93.01`).

## Reproducible workflow

The tested sequence was:

1. Resolve a model to machine and OTA ID with TOH/caniroot.
2. Resolve a firmware file from an official LG page or the archive's
   `processed_fw.json`; record firmware version, source, size, ZIP SHA-256, and EPK
   SHA-256.
3. Build `openlgtv/epk2extract` and run it against the EPK. Current master includes
   AES/RSA material for the analyzed older families and the webOS 25
   `o22n3`/`o24n`/`k24n`/`k25lp` families.
4. For images that contain `super.pak`, parse `dpmeta.pak`, carve the logical
   partitions out, and run `epk2extract` on each resulting squashfs image. The older
   images analyzed here emitted `rootfs.pak` and `bsppart.pak` directly.
5. Run `webosbrew-fw-symbols-extract` against the directory named
   `<firmware-version>-<OTA-ID>`.
6. Run a project-specific pass over the GStreamer plugin directory and the five
   gate artifacts, recording hashes, build IDs, ELF attributes, `DT_NEEDED`, package
   ownership/version, and relevant config deltas.

The public helper
[`unpack-super.sh`](https://github.com/gprot42/webos-scripts-public/blob/main/unpack-super.sh)
worked unchanged for the LX G5 image. It currently hardcodes logical partitions on
`/dev/mmcblk0p28`; the Realtek B5 image uses `/dev/mmcblk0p29`. General automation
must parse the device path from `dpmeta.pak` instead of fixing it to `p28`.

## Tool assessment

### `epk2extract`

Works for the six inspected EPK v3 images (G5, B5, C2, C3, B2, and B3). The tested revision was
`94c95fdf6d189bf177cc2e126579190531d84c51`.

It provides the essential decryption, package extraction, and squashfs extraction.
It does not provide a compatibility verdict; that policy belongs in this project.

### `dev-toolbox-cli`

Useful for package versions and top-level ELF sonames, dependencies, and exported
symbols. The tested revision was
`ea38b20ced68124fa914ac8625cc072ad94e39b2` (`0.9.0`). It emitted:

| Firmware | Library index entries | Package entries |
|---|---:|---:|
| G5 `W25O` | 1,881 | 1,878 |
| B5 `W25H` | 1,735 | 1,787 |

For this project it has an important gap: `fw-extract` scans `lib`, `usr/lib`, and
the directories listed by `ld.so.conf`, but it does not recurse into
`/usr/lib/gstreamer-1.0`. Consequently it emitted no JSON for
`libgstlibav.so`, `libgstisomp4.so`, or `libgstmpegtsdemux.so`.

Its per-library JSON also contains package, `DT_NEEDED`, and symbol data, but no
content hash, build ID, ELF architecture/float ABI, or GStreamer factory/caps data.
A small project-specific analyzer is still required unless those fields and plugin
directories are added upstream.

### TOH / caniroot

Useful for model fan-out, machine, OTA ID, codename, and region. The tested caniroot
revision was `70067eb37140037f4071d994fa949c62babbcd8d`.

The table maps identity; it does not establish media-pipeline compatibility. Use
the full OTA ID and firmware version as manifest keys, not a marketing model name or
only the webOS major release.

## What firmware analysis can and cannot prove

It can prove:

- loader, ELF class/machine/float ABI, and required symbol versions;
- package versions and SoC-specific package revisions;
- byte identity or drift of every file the payload shadows;
- presence and static dependencies of GStreamer/core media components;
- config schema and exact config deltas; and
- whether a target is worth requesting from a tester.

It cannot prove:

- successful registry scanning under the TV's real environment;
- `decodebin`/`decproxy` factory selection during Media Player playback;
- closed `umediaserver` resource acquisition and stream-content validation;
- sink/HAL acceptance of the decoder's negotiated PCM format and channel count;
- audible output, eARC behavior, or downstream downmixing; or
- reboot, Disable, Uninstall, and firmware-drift behavior on that target.

The archived DTS-retag hardware experiment is the concrete warning: source and caps
analysis predicted a coherent path, but the real closed resource manager rejected
the stream content before the converter could run.

## Recommended compatibility policy

1. **Separate artifact compatibility from target verification.** Keep an artifact
   set keyed by GStreamer ABI and stock hashes, and a second target allowlist keyed
   by OTA ID/product family plus firmware range.
2. **Do not call a firmware-only match `verified`.** For example, G5 can be shown as
   "firmware match: exact C5 media artifacts; hardware test pending".
3. **Preserve default refusal for targets without hardware approval.** The C2
   implementation demonstrates the intended policy: an exact firmware/artifact
   match may expose the two-step tester opt-in, while a partial identity/version/hash
   match and every diagnostic-only family remain non-forceable.
4. **Keep on-device checks authoritative.** Loader resolution, post-bind plugin
   inspection, registry commit gating, and drift stand-down remain required even
   when a firmware manifest matches.
5. **Record results by OTA ID and firmware version.** A later LG update can change
   the rootfs without changing the marketing model.
6. **Test one representative per machine/OTA family.** A G5 test can promote the
   analyzed `o24n/W25O` artifact set; it must not automatically promote `k24n` or
   `k25lp`.

For the landed C2 profile, the required promotion test is an exact global C2/G2
`W22O` on `04.40.93`/`04.40.93.01`: capture detector output and pristine SHA-256,
run Enable plus the MP4 self-test, play real MKV and MP4 DTS, reboot, Disable, and
Uninstall. Do not test TS/M2TS or TrueHD as if this payload supported them.

For webOS 25 expansion, the next requested hardware target remains a G5/M5 on
`W25O`, followed by a B5 or other `W25H` set under the explicit experimental
opt-in. Capture detector output, pristine stock fingerprints, loader preflight,
self-test, real MKV/MP4/TS DTS and TrueHD playback, Disable, and one reboot cycle.
