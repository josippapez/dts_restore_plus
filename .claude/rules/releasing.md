# Rule: releasing & prebuilt-binary hygiene (webOS 25)

**Applies to:** any change under `webos25/` in this fork (`dts_restore_plus`).

The shipped `.so` are **vendored, committed, and on-device-verified**, and they are
the source of truth. The release GitHub Action **packages** them on a version tag —
it does **not** rebuild from source. Two obligations follow:

1. **Keep the committed binaries in sync with their source.** If you change a
   binary-affecting file, you MUST rebuild, re-verify on a real webOS-25 TV, and
   re-commit the `.so` in the SAME change. Binary-affecting files:
   - the sources of the committed binaries, all in `webos25/restore/src/`:
     `gstdtsdec.c`, `gstdtsdec.h`, `dtsdec-track.h`, `lgroute.c`, `lgroute.h`,
     `lgroute-graph.h`, `lgroute-control.c`, `lgroute-control.h`, `gstadecswitch.c`,
     `gstadecswitch.h`, `edidaudio.c`, `edidaudio.h`
   - the `dts_support` patch / pinned sources / flags in `webos25/restore/build-demux.sh`
   - `webos25/restore/build.sh`, `build-truehd.sh`, `build-demux.sh`, `build-switch.sh`, `build-edid.sh`, `build-ts114.sh`
   - the kernel module: `webos25/kmod/earc_route.c`, `earc_decode.h`, `earc_hdmi.h`,
     `earc_probe.h`, `build.sh`, `build.py`

   The committed binaries live in
   `webos25/restore/{out,truehd-out,demux-out,switch-out,edid-out,ts114-out,kmod-out}`.
   (`out/` holds `libgstdtsdec.so` and `lgroute-control`; `kmod-out/` holds
   `earc_route-5.4.ko` and `earc_route-6.12.ko`, copied from `webos25/kmod/out/`.)
   The app's `webos25/app/payload/**` `.so` are git-ignored and copied from
   `webos25/restore/**` at package time — never edit them there.

2. **Publish by tagging.** After any change you want released (binary, installer,
   or app), cut a release: `git tag webos25-<X.Y> && git push origin webos25-<X.Y>`.
   The **Release (webOS 25)** workflow builds the CLI tarball + the `.ipk` and
   publishes them. Confirm the run is green and both assets are attached.

Do NOT hand-upload release assets or let the release drift behind the committed
binaries. Full procedure: [`RELEASING.md`](../../RELEASING.md). Agents: use the
`release-webos25` skill.
