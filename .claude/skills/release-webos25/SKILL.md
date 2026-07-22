---
name: release-webos25
description: Cut a webOS 25 release for this fork (dts_restore_plus) — verify the committed prebuilt binaries are current, rebuild + re-verify them on-device if any binary-affecting source changed, then tag to trigger the release-on-tag GitHub Action that packages the CLI tarball + builds the .ipk. Use whenever publishing a release, or after changing the DTS/TrueHD/demuxer sources, build scripts, install.sh/init_dts25.sh, or the app.
---

# Release a webOS 25 build (`dts_restore_plus`)

Authoritative details live in [`RELEASING.md`](../../../RELEASING.md). This skill is
the checklist to follow.

## Core rule

The shipped binaries are **vendored + on-device-verified**, committed under
`webos25/restore/{out,truehd-out,demux-out}`. The release Action **packages** these
committed binaries — it does **not** rebuild from source. So they must be current
before you tag. The app's `webos25/app/payload/**` `.so` are git-ignored and copied
from `webos25/restore/**` at package time.

## Decide: do the binaries need a rebuild?

Rebuild + re-verify + re-commit the `.so` **only if** the change touched something
that affects them:

- `webos25/restore/src/gstdtsdec.c` (DTS patch)
- the `dts_support` patch or pinned sources/flags in `webos25/restore/build-demux.sh`
- `webos25/restore/build.sh` / `build-truehd.sh` / `build-demux.sh` (toolchain/ABI)

Edits to `install.sh` / `init_dts25.sh` / the app (JS/HTML/CSS) do **not** need a
rebuild — but still cut a release so the assets carry the change.

## Steps

1. **If binaries affected:** rebuild in `webos25/restore/`:
   `./build.sh`, `./build-truehd.sh`, `./build-demux.sh`. Confirm each ABI report
   shows **ARM EABI5 soft-float `0x05000200`, `ld-linux.so.3`, GLIBC ≤ 2.35**.
2. **If binaries affected:** verify on a **real webOS-25 TV** — install and play a
   DTS MKV, an mp4/ts/m2ts DTS file, and a TrueHD MKV. Commit the new `.so` only
   after it plays.
3. Bump the app version in `webos25/app/appinfo.json` and
   `webos25/app/service/package.json` if the app changed. Update
   `webos25/README.md` / `RELEASING.md` as needed.
4. Ensure everything is committed and on `main`.
5. **Tag to release:**
   ```sh
   git tag webos25-<X.Y>      # e.g. webos25-1.2  (or v<X.Y.Z>)
   git push origin webos25-<X.Y>
   ```
6. Watch the **Release (webOS 25)** Action. When green, confirm the release has
   both assets: `dts_restore_plus-webos25-restore-*.tar.gz` and
   `io.github.josippapez.dtsenabler_*_all.ipk`.

## Verify locally without tagging (optional)

Package the `.ipk` locally per the "Package the `.ipk` locally" section of
`RELEASING.md` to sanity-check before tagging.
