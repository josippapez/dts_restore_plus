# Releasing (webOS 25 / `dts_restore_plus`)

This fork ships **prebuilt, on-device-verified binaries**. Releases are cut by
**pushing a version tag** — a GitHub Action (`.github/workflows/release.yml`)
then packages the committed binaries and publishes the assets. **It does not
rebuild the `.so` from source.** The cross-compiled binaries are the source of
truth in the repo, so they must be current *before* you tag.

## The rule (read this before changing anything binary-affecting)

The single source of truth for the shipped binaries is:

```
webos25/restore/out/         libgstdtsdec.so, libdca.so.0        (DTS decoder)
webos25/restore/truehd-out/  libgstlibav.so + libav*/libsw*      (TrueHD/MLP)
webos25/restore/demux-out/   libgstisomp4.so, libgstmpegtsdemux.so (mp4/ts/m2ts DTS)
```

The app's `webos25/app/payload/**` `.so` are **git-ignored** and are copied from
`webos25/restore/**` at package time — so you only ever update the binaries in
`webos25/restore/`.

**If you touch anything that affects those binaries, you MUST rebuild + re-verify
+ re-commit them, then re-release.** "Affects those binaries" includes:

- `webos25/restore/src/gstdtsdec.c` (the DTS patch)
- the `dts_support` demuxer patch or its version, in `webos25/restore/build-demux.sh`
- `webos25/restore/build.sh`, `build-truehd.sh`, `build-demux.sh` (toolchain,
  flags, pinned sources, ABI)

Editing `install.sh` / `init_dts25.sh` / the app JS/HTML does **not** require a
rebuild — but still cut a new release so the tarball/`.ipk` carry the change.

## The boot script's three copies must match

The DTS+TrueHD boot script exists in three places:

1. `webos25/restore/init_dts25.sh` — canonical, and the **only** one a human edits.
2. The base64 `INIT_B64` heredoc payload in `webos25/restore/install.sh` — generated.
3. The `W25_INIT_HEAD`/`W25_COMPAT_SH`/`W25_INIT_MAIN` arrays that
   `w25InitScriptBody()` renders in `webos25/app/service/service.js` — generated.

If they drift, the CLI installer, an already-installed TV and the Homebrew app end up
running **different** boot scripts. See `CLAUDE.md` rule 3.

**Changing the boot script — including adding a verified TV to the gate's table — is a
two-step edit:**

```sh
# 1. edit webos25/restore/init_dts25.sh only, then:
sh webos25/restore/sync-init.sh          # regenerate copies 2 and 3 from it
sh webos25/restore/check-init-sync.sh    # verify, the same way CI does
```

`sync-init.sh` rewrites the `install.sh` blob and the three `service.js` arrays from the
canonical file. It refuses to write anything unless it can locate every target precisely
— it computes both files in memory first, so a failure on either blocks writes to both
rather than half-writing one — and it is idempotent (a second run reports "already in
sync" and changes nothing). Hand-editing a 30 KB base64 blob is not a reviewable change;
don't.

It also refuses, writing nothing, if the canonical script's body changed since the last
sync while its hand-maintained `W25_GATE_VERSION` stamp did not. That stamp is how the app
notices an installed boot script is older than the one it ships (`hookStale`), so an
un-bumped stamp would make that detection go quiet on exactly the TVs that need it. **Bump
`W25_GATE_VERSION` whenever you change the script's behaviour, then run `sync-init.sh`.**
The app also md5-compares what Enable would write against what is installed, so a stale
stamp is caught at runtime too — but the generator is where it should be caught.

If `check-revert-sync.sh` exits **2** rather than 1, that is a setup error, not a violated
invariant: its extractor stopped matching one of the three scripts, almost always because
the unmount loop was reformatted. Fix the extractor (or the formatting) — an exit 2 means
the guard has stopped guarding, which is worse than a failure.

`check-init-sync.sh` decodes copy 2, evaluates copy 3 without loading `webos-service`,
and diffs both against copy 1, printing a unified diff and naming whichever drifted. It
also calls `webos25/restore/check-revert-sync.sh`, which covers a *different* duplication
the three copies don't include: the set of bind targets `install.sh`/`uninstall.sh`
unmount, and the `GST_PLUGIN_PATH_1_0` used to regenerate a clean **stock** registry. It
compares extracted sets and values rather than whole lines, so formatting or variable
renames never false-fail — only a genuinely missing bind target or a diverged registry
path does. A missing bind target in `uninstall.sh` is exactly the "not fully reversible"
defect the compatibility work fixed, which is why it is guarded rather than trusted.

The release workflow runs `check-init-sync.sh` before packaging, so a drifted release
fails instead of shipping mismatched boot scripts. Both scripts need only POSIX `sh`,
`node`, `base64` and `diff` — all present on macOS, Linux and the CI image.

## License texts ship inside both artifacts

`webos25/app/licenses/` (`GPL-2.0.txt`, `LGPL-2.1.txt`, `NOTICE.md`) is committed and is
picked up automatically by `ares-package`, so a local `.ipk` build carries it too. The
release workflow copies it into `restore/` before tarring so the CLI tarball ships the
same texts; that copy is gitignored build output, never a second source. GPL-2.0 and
LGPL-2.1 both require handing recipients a copy of the license, and the DTS decoder
(`libgstdtsdec.so` + `libdca.so.0`) is GPL-2.0-or-later — see the per-artifact table in
the root `README.md`.

## Rebuild + verify (only when the binaries are affected)

```sh
cd webos25/restore
./build.sh          # -> out/libgstdtsdec.so, out/libdca.so.0
./build-truehd.sh   # -> truehd-out/libgstlibav.so + libav*/libsw*
./build-demux.sh    # -> demux-out/libgst{isomp4,mpegtsdemux}.so
```

Each build prints an ABI report — **confirm ARM EABI5 soft-float
(`e_flags 0x05000200`), interpreter `ld-linux.so.3`, max GLIBC ≤ 2.35** before
trusting the output. Then **verify on a real webOS-25 TV** (install, play a DTS
MKV + an mp4/ts/m2ts DTS file; a TrueHD MKV **and** a TrueHD `.ts`/`.m2ts`).
Only then commit the new `.so`.

The TrueHD `.ts`/`.m2ts` case is easy to skip and easy to be fooled by: BD TrueHD
carries an AC-3 compatibility substream on the **same PID**, so if the TrueHD pad is
not exposed the AC-3 core decodes and playback sounds perfectly normal. Confirm the
audio-track list actually offers a TrueHD track, or that the decoder negotiates the
**side**-pair channel mask (`0x0c0f`) rather than AC-3's **rear** pair (`0x003f`).
An **ffmpeg-muxed** TS cannot verify this — ffmpeg does not write BD PES substream
framing, so TrueHD comes out as 2 channels and decodes to nothing. Use tsMuxeR or a
straight disc copy.

## Cut a release

1. Make sure the binaries in `webos25/restore/**` are current and committed
   (rebuilt + verified if they were affected — see above).
2. Bump the app version in `webos25/app/appinfo.json` +
   `webos25/app/service/package.json` if the app changed.
3. Tag and push — the workflow does the rest:

   ```sh
   git tag webos25-1.2        # or v1.2.0
   git push origin webos25-1.2
   ```

   The Action packages `dts_restore_plus-webos25-restore-<tag>.tar.gz` (the CLI:
   `restore/` with `install.sh`) **and** builds
   `io.github.josippapez.dtsenabler_<ver>_all.ipk` (the app), then publishes a GitHub
   release with both. You can also trigger it manually from the Actions tab
   (workflow_dispatch, supply the tag name).

## Package the `.ipk` locally (optional / debugging)

```sh
cd webos25/app
# populate payloads from the committed restore/ binaries (they're git-ignored here)
cp -f  ../restore/out/libgstdtsdec.so ../restore/out/libdca.so.0        payload/webos25/
cp -Pf ../restore/truehd-out/libgstlibav.so ../restore/truehd-out/libav*.so* \
       ../restore/truehd-out/libsw*.so*                                 payload/webos25-truehd/
cp -f  ../restore/demux-out/libgstisomp4.so ../restore/demux-out/libgstmpegtsdemux.so \
                                                                        payload/webos25-demux/
npm install -g @webosose/ares-cli
ares-package . service -o dist        # -> dist/io.github.josippapez.dtsenabler_<ver>_all.ipk
```

## Checklist

- [ ] Binaries in `webos25/restore/**` current (rebuilt + on-device-verified if affected)
- [ ] `webos25/restore/demux-out/BUILD-REPORT.txt` reflects the current build
- [ ] `sh webos25/restore/check-init-sync.sh` passes (the release workflow also runs it)
- [ ] App version bumped (if the app changed)
- [ ] Docs updated (`webos25/README.md`, this file)
- [ ] Tag pushed → release workflow green → assets present on the release
