---
name: add-tv-generation
description: Add support for a new LG TV generation or model to dts_restore_plus — inspect its firmware without owning the TV, decide which restore mechanism applies, build any binary it needs, gate it exactly, and ship it without regressing a working profile. Use when someone reports an unsupported TV, when widening an existing profile to another firmware, or when deciding whether a generation is viable at all.
---

# Add a TV generation to `dts_restore_plus`

Most of this is answerable from the **firmware image alone**, before anyone tests
anything. Do that first: it is faster than a round-trip with an owner, and it stops
you shipping a profile that could never have worked.

Read [`webos25/docs/MULTI-MODEL.md`](../../../webos25/docs/MULTI-MODEL.md) §1–2 for
the existing decision matrix and per-target recipes before adding a new row.

## 1. Get the firmware

The archive index is `https://lg.slada.sk/processed_fw.json` (~3 MB).

- **Match on `firmwareotaID`, never the version string.** LG reuses version numbers
  across boards: `23.25.55` is a C2/CS `HE_DTV_W22O_AFABATPU` build in the UK/US
  mirrors *and* a Realtek B3 `W23H` build in the JP mirror. Same number, different TV.
- Download from **`https://tv.slada.sk/<relativePath>`**. `lg.slada.sk` is a SvelteKit
  SPA that builds links client-side, so they are absent from the served HTML and every
  path guess returns the same ~25 KB page. If you need to see a link, use
  `agent-browser` and read the `href`s.
- `curl` needs **`--globoff`**: the filenames contain parentheses.
- LG's own `gscs-b2c.lge.com/downloadFile?fileId=<id>` (id = the prefix before `---`)
  returns **403**.
- Verify the download against the index's `zipFileSha256` before trusting anything
  you read out of it.

```sh
curl -sL --globoff -o fw.zip 'https://tv.slada.sk/mirror2/UK/<id>---Software_File(Version_<v>).zip'
unzip -q fw.zip -d unz && ./epk2extract unz/*.epk
```

`epk2extract` (github.com/openlgtv/epk2extract, `./build.sh`) must print
**`Trying AES Key`** lines. None means zero keys loaded — see the
`epk2extract-key-path-gotcha` memory. The rootfs is **zstd** squashfs, so use
Homebrew `squashfs`; the bundled unsquashfs silently emits only `.pak`.

**`unsquashfs -l rootfs.pak` lists the whole tree without extracting**, which answers
most questions below for the price of the download alone.

## 2. Decide the mechanism from the image

Three things decide everything. Extract them and stop guessing.

| Check | Command on the extracted rootfs | Meaning |
|---|---|---|
| Is there a DTS decoder? | `strings usr/lib/gstreamer-1.0/libgstlibav.so \| grep -c 'Invalid number of primary audio channels'` | decoder-internals string, not a descriptor. 0 means absent even if `avdec_dca` appears in config |
| Is LG's decoder DTS-capable? | `strings usr/lib/gstreamer-1.0/libgstlgaudiodec.so \| grep -i 'x-dts\|LGADEC_CODEC_DTS'` | present = native path exists (C3/C4); absent = it was removed (C5) |
| Are the demuxers nerfed? | `strings usr/lib/gstreamer-1.0/libgstmatroska.so \| grep -c 'audio/x-dts'` | 0 = nerfed (CX). Non-zero = fine, the decoder is the problem |
| What does config claim? | `grep -E 'dts_audiodec\|avdec_dca' etc/gst/gstcool.conf` | a rank entry is **not** proof of a decoder. C2 lists `avdec_dca=0` for a decoder that is not in the build |

Beware the **false-capability trap**: `device_codec_capability_config.json` still lists
`DTS`/`DTSE`/`DTSH` on sets where LG removed the decoder entirely, and a registered
`dts_audiodec` element is not proof either. Trust decoder-internals strings.

Then pick:

- **decoder present, demuxer nerfed** → demuxer-override (CX).
- **decoder absent, demuxers fine** → inject a decoder (C2, webOS 25).
- **decoder present and working, only a demuxer gate off** → flip the gate (C3).
- **no decoder anywhere and no loadable payload** → refuse with a diagnostic profile.
  A GStreamer version match alone never justifies inheriting another family's mechanism.

## 3. The plugin ABI rule — this is what makes reuse possible

`gst_plugin_check_version()` rejects a plugin only when
`major != GST_VERSION_MAJOR || minor > GST_VERSION_MINOR`
(`gstplugin.c:487` in the webOS-25 tree).

**An older-minor plugin loads on a newer core.** That is why 1.14.4 binaries run on a
1.18.5 C2, and it usually means you do **not** need that generation's LG source. Check
what LG actually publishes before assuming you need it: the `lgstreamer` org has only
1.14.4 (CX) and 1.24.0 (webOS 25) — **no 1.18 tree exists**.

Also check whether the feature is even gated in the older tree. LG's `dts_support`
gate around TS DTS exists in 1.24 but **not** in 1.14.4, so the 1.24 build needs a
source patch and the 1.14.4 build is a plain compile of published source.

## 4. Build, if you need to

Model on the closest existing script rather than writing a new one:

- `webos25/restore/build-demux.sh` — demuxers, meson, `debian:11-slim`, armel
- `webos25/restore/build-truehd.sh` — a `libgstlibav.so` with ffmpeg statically linked
- `webos25/restore/build-ts114.sh` — a 1.14.4 tree built for an older ABI

Target ABI for these sets: ELF32 ARM EABI5, `e_flags 0x05000200` (soft-float),
`/lib/ld-linux.so.3`. Verify with `file`, `od -An -tx4 -j36 -N4`, `readelf -d`, and
compare max GLIBC (`objdump -T | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1`) against
a binary already known to run on that TV.

**Era-appropriate tooling matters.** meson 1.4.2 cannot configure a 1.14.4 tree; use
bullseye's meson 0.56.2 or autotools. Expect a few upstream defects in old trees and
fix them at build level, never by patching the vendored source.

## 5. Gate it exactly

Copy the C2 posture (`service.js`, `C2_EXPECTED_SETS`). A gate is a **list of known
images**, not a loosened range:

- accept explicit firmware + webOS + GStreamer **triples**, so a mixed pair never matches;
- match the three stock plugin hashes as a **set**, never field-by-field across sets, or
  a chimera of two firmwares passes;
- name the failing gate in the refusal. "One of seven gates mismatched" is unactionable
  and cost several round-trips before it was fixed;
- adding a firmware to the list does **not** promote the profile. Hardware verification
  is separate.

## 6. Never regress a working profile

The single most important rule when widening an existing profile.

- Anything unproven on the target is an **optional** bind: if it cannot stage, or its
  loader trace reports `not found`, record a skip reason and **return success**. The
  mandatory set keeps working. See `c2_payload`'s TS handling.
- Add a new element to the registry proof list **only when it actually bound**.
- Put per-generation payloads in their **own** directory. `payload/cx/` is shared with
  the CX profile; adding a file there changes CX behaviour.
- Every enable/disable path must stay reversible, and `Disable` must not depend on the
  state being tidy.
- Cover all four paths in `app/service/test/profile-compat.test.js`: not shipped,
  dependency unresolved, usable, and no stock target to override. Then make the change
  hard-fail locally and confirm the test goes red. A guard that cannot fail is not a guard.

## 7. Make failures diagnosable before asking an owner to test

Every round-trip costs hours. Aggregate messages cost several.

- Log the **inputs** behind a verdict, not just the verdict: the measured values, the
  mount states, and the raw `REFUSED=`/`REASON=` output of the apply.
- Name the specific file and missing library, never "one of four failed".
- The log lives at `/var/lib/webosbrew/dtsenabler/dtsenabler.log`. Do not use `/tmp`;
  it is cleared on these TVs, which is why an owner asked for a log and found nothing.
- Remember the UI toast auto-hides after ~5s, so anything only shown there is lost.

## 8. Ship and verify

Follow [`.claude/rules/releasing.md`](../../rules/releasing.md) and the
`release-webos25` skill. A binary that has not run on real hardware ships **wired into
nothing** — committed, but referenced by no payload, installer, or release path.

For an unverified generation, the honest sequence is: build → gate → optional bind →
release → owner tests → only then promote wording, self-test coverage, and docs.

## Mistakes this project has actually made

Each of these cost real time; none is hypothetical.

1. Trusting a **descriptor or config entry** as proof of a decoder. Use decoder-internals strings.
2. Assuming a registered element means a working codepath. On C2, `dts_audiodec` is registered over a 128 KB stub.
3. Letting a **behavioral guess override an exact match** — a registered `dts_audiodec` relabelled an exact-matched C2 as `native-dts-gated` and hid its opt-in.
4. Making a value expand in one generated script and forgetting the others. Every C2 script runs under `set -u`; an unbound variable kills the probe and everything after it silently returns empty.
5. Verifying a change against one generated script only. Generate **all** of them and `sh -n` each.
6. Deriving a mount's expected source from the **longest** mountpoint prefix. If `/var` is its own filesystem, that is the wrong device and your own mounts read `foreign`.
7. Comparing an unresolved path against a mount table that records **resolved** paths.
8. Shipping a new payload file as mandatory. It takes away a working install on any TV that lacks its dependencies.
