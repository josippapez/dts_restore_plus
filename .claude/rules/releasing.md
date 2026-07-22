# Rule: releasing & prebuilt-binary hygiene (webOS 25)

**Applies to:** any change under `webos25/` in this fork (`dts_restore_plus`).

The shipped `.so` are **vendored, committed, and on-device-verified**, and they are
the source of truth. The release GitHub Action **packages** them on a version tag —
it does **not** rebuild from source. Two obligations follow:

1. **Keep the committed binaries in sync with their source.** If you change a
   binary-affecting file, you MUST rebuild, re-verify on a real webOS-25 TV, and
   re-commit the `.so` in the SAME change. Binary-affecting files:
   - `webos25/restore/src/gstdtsdec.c`
   - the `dts_support` patch / pinned sources / flags in `webos25/restore/build-demux.sh`
   - `webos25/restore/build.sh`, `build-truehd.sh`, `build-demux.sh`

   The committed binaries live in `webos25/restore/{out,truehd-out,demux-out}`.
   The app's `webos25/app/payload/**` `.so` are git-ignored and copied from
   `webos25/restore/**` at package time — never edit them there.

2. **Publish by tagging.** After any change you want released (binary, installer,
   or app), cut a release: `git tag webos25-<X.Y> && git push origin webos25-<X.Y>`.
   The **Release (webOS 25)** workflow builds the CLI tarball + the `.ipk` and
   publishes them. Confirm the run is green and both assets are attached.

Do NOT hand-upload release assets or let the release drift behind the committed
binaries. Full procedure: [`RELEASING.md`](../../RELEASING.md). Agents: use the
`release-webos25` skill.
