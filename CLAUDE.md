# CLAUDE.md — project guidance for `dts_restore_plus`

Fork of [`lgstreamer/dts_restore`](https://github.com/lgstreamer/dts_restore). The CX
(webOS 3–6) tool is carried over from upstream; the active work is **webOS 25** under
`webos25/`.

## Layout

- `webos25/restore/` — the CLI: prebuilt decoders (`out/`, `truehd-out/`) + container
  demuxers (`demux-out/`) + `install.sh`/`uninstall.sh` + `build*.sh`. One-command
  install on the TV: `sh install.sh`.
- `webos25/app/` — the "DTS Enabler" Homebrew app (enable/disable/uninstall + self-test
  + play-by-ear). Its `payload/**/*.so` are **git-ignored** and vendored from
  `webos25/restore/**` at packaging time.
- `gst/` — upstream CX binaries (unchanged).

## Non-negotiable rules

1. **Prebuilt binaries are vendored + on-device-verified.** The committed `.so` in
   `webos25/restore/{out,truehd-out,demux-out}` are the source of truth. If you change
   a binary-affecting file (`webos25/restore/src/gstdtsdec.c`, the `dts_support` patch or
   pins in `build-demux.sh`, or any `build*.sh`), you MUST rebuild, **verify on a real
   webOS-25 TV**, and re-commit the `.so` in the same change. See
   [`.claude/rules/releasing.md`](.claude/rules/releasing.md) and [`RELEASING.md`](RELEASING.md).
2. **Release by tagging.** After any change to be shipped, cut a release:
   `git tag webos25-<X.Y> && git push origin webos25-<X.Y>` → the **Release (webOS 25)**
   Action packages the CLI tarball + builds the `.ipk` and publishes. Agents: use the
   `release-webos25` skill. Don't let the release drift behind the committed binaries.
3. **Keep the app mechanism in sync with `install.sh`.** `webos25/app/service/service.js`
   mirrors `webos25/restore/install.sh` + `init_dts25.sh`. Change both together.
4. **Homebrew Channel exec bridge:** its `/exec` returns `stdoutString`/`stderrString`
   (not `stdout`/`stderr`). Read the `*String` fields. Run decode/inspect with
   `GST_REGISTRY_FORK=no` so the plugin-scanner fork doesn't hold the exec pipe open.

## On-device testing

Root SSH to the rooted C5 is how everything is verified. Prefer the CLI `install.sh`
path for ground-truth; the app delegates the same shell to the exec bridge.
