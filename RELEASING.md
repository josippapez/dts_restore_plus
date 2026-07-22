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
MKV + an mp4/ts/m2ts DTS file; TrueHD MKV). Only then commit the new `.so`.

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
- [ ] App version bumped (if the app changed)
- [ ] Docs updated (`webos25/README.md`, this file)
- [ ] Tag pushed → release workflow green → assets present on the release
