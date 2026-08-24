# DESIGN — TS/M2TS containers for the C2/G2/CS profile

Status: **BINARY BUILT, NOT DEVICE-VERIFIED, NOT INTEGRATED**. Scoped and built
2026-08-24. The artifact is `webos25/restore/ts114-out/libgstmpegtsdemux.so`
(246.7 KB, sha256 `c2fb3c63677cd267820984f37173772cb281929422099eb1b00e3707479dab1d`),
built by `webos25/restore/build-ts114.sh`. It is wired into nothing: no payload, no
installer, no release path, per [`../../.claude/rules/releasing.md`](../../.claude/rules/releasing.md).

## The blocker is the container, not the channel count

The app bundles three DTS samples. Measured with `ffprobe`:

| file | codec | profile | channels | plays on C2 |
|---|---|---|---|---|
| `DTS-in-mp4.mp4` | dts | DTS-HD MA | 6 — `5.1(side)` | **yes** |
| `DTS-HD-MA-5.1.ts` | dts | DTS-HD MA | 6 — `5.1(side)` | no |
| `DTS-HD-MA-5.1.m2ts` | dts | DTS-HD MA | 6 — `5.1(side)` | no |

All three are 6-channel DTS-HD MA, so "these files are multichannel" does **not**
explain the failures: a 5.1 file already plays in MP4 on that TV today (arriving as
2.0, because the payload decoder downmixes — see
[`DESIGN-C2-MULTICHANNEL.md`](DESIGN-C2-MULTICHANNEL.md)).

The only difference is the container. `payload/cx/` ships `libgstlibav.so`,
`libgstisomp4.so`, `libgstmatroska.so` and `libgstisomp4_1_8.so` and **no TS
demuxer** — `MULTI-MODEL.md` §2.3 states "No legacy `libgstmpegtsdemux.so` is
packaged", and §2.1 lists it as the one CX binary never built.

## LG's 1.14.4 TS demuxer needs no patch

The webOS-25 (1.24) work needs a source patch because LG wrapped upstream's DTS
handling in `#ifdef DTS_SUPPORT` plus a runtime `demux->dts_support`, defaulting
FALSE (`tsdemux.c:1179`), and `build-demux.sh` flips that default.

**That gate does not exist in LG's 1.14.4 tree.** In
`lgstreamer/gst-plugins-bad@lg`, `gst/mpegtsdemux/tsdemux.c` contains zero
occurrences of `DTS_SUPPORT`, its sink template advertises
`audio/x-dts; audio/x-dtsh; audio/x-dtse; audio/x-dtsl` (line 531), and
`case ST_PS_AUDIO_DTS:` sets `audio/x-dts` unconditionally (line 3414). The
`DRF_ID_DTS1/2/3/DTSH` registration paths and `set_caps_for_private_dts()` are
present too. LG added the gate somewhere after 1.14.4.

So this is a **plain build of published source**, with no patch step at all.

## Why a 1.14 plugin loads on a 1.18 TV

`gst_plugin_check_version()` rejects a plugin only when
`major != GST_VERSION_MAJOR || minor > GST_VERSION_MINOR` (`gstplugin.c:487`).
14 ≤ 18, so it loads — which is already demonstrated: the four 1.14.4 binaries in
`payload/cx/` are running on that owner's 1.18.5 TV right now.

Keeping LG's own tree (rather than upstream 1.18) also keeps LG's TS extensions —
`app_type`/RTC, dtcpip, miracast LPCM and the LG descriptor paths — which an
upstream demuxer would drop, with unknown effect on live TV and DVR.

## Build — as actually done

`webos25/restore/build-ts114.sh`, modelled on `build-demux.sh`: `debian:11-slim`,
`dpkg --add-architecture armel`, snapshot pin `20250601T000000Z`,
`gcc-arm-linux-gnueabi`, meson cross file. Source is `lgstreamer/gst-plugins-bad`
branch `lg` (1.14.4) built against matching 1.14.4 `gstreamer` and
`gst-plugins-base`. `tsdemux.c` compiled **untouched**, and the script now asserts
the no-patch premise (zero `DTS_SUPPORT`, the sink caps template, the
`ST_PS_AUDIO_DTS` case) on the host and again in-container, so a future `lg`
revision that adds the gate fails the build instead of silently shipping.

Three findings worth keeping:

- **meson 1.4.2 cannot configure a 1.14.4 tree.** `build-demux.sh`'s pip pin had to
  be replaced with bullseye's own **meson 0.56.2** from apt. The hazard was real.
- Three upstream/LG defects in the 1.14.4 tree needed build-level workarounds, none
  of them a source patch: `-Dwith-ptp-helper-permissions=none` crashes core's own
  post-install script (used setuid-root instead); `gst-plugins-base`'s `use_orc=no`
  branch never defines the `orc_dep` that `audiomixer` references (left orc on auto
  with orc absent); and `gst-libs/gst/basedrm/meson.build` closes its `endif` before
  the `declare_dependency` that uses `gstbasedrm` — the **same** defect, with the
  same one-line fix, that `build-demux.sh` already applies to `gst-libs/gst/mpdclient`.
- The build is bit-for-bit reproducible: the container image was torn down and the
  rebuild produced an identical sha256.

## Deployment

Per-target payload, matching the multichannel plan:

- add `libgstmpegtsdemux.so` to a C2-specific payload directory;
- bind it over `/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so` only for
  `webos22-o22-gst118`;
- extend `c2_payload` from four files to five, and `c2_apply`/`c2_detach`
  accordingly — the existing ownership, drift and recovery checks then cover it
  with no new mechanism;
- update the profile's advertised capability: it currently reports "MP4 active
  (TS/M2TS unavailable)" (`app/js/app.js`) and the self-test is MP4-only, both of
  which would become wrong.

## Unverified — do not state these as facts

1. **Does a 1.14.4 TS demuxer actually work against a 1.18.5 core at runtime?**
   It will *load* (ABI rule above). Whether tsdemux's interaction with the 1.18
   `mpegtsbase`/`mpegtspacketizer` in the same plugin is sound is untested — note
   the whole `mpegtsdemux` plugin is one `.so`, so `mpegtsbase` comes along with it
   and the pairing is internally consistent.
1b. ~~**Two library dependencies are additions.**~~ **RESOLVED 2026-08-24 from the
   owner's own firmware image.** Measured `NEEDED` adds `libgstcodecparsers-1.0.so.0`
   and `libgstmpegts-1.0.so.0` versus the `gst/libgstmatroska.so` baseline, and both
   are present on that generation:

   ```
   /usr/lib/libgstcodecparsers-1.0.so.0 -> libgstcodecparsers-1.0.so.0.1805.0
   /usr/lib/libgstmpegts-1.0.so.0       -> libgstmpegts-1.0.so.0.1805.0
   ```

   The `.1805.0` suffix is the GStreamer 1.18.5 build, matching the TV's core, and
   both ship as opkg packages (`lib32-libgstcodecparsers-1.0-0`,
   `lib32-libgstmpegts-1.0-0`). The image also carries a stock
   `/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so` for ours to override. Max GLIBC is
   `GLIBC_2.7`, equal to the baseline, not higher. So the plugin should load.
2. **Live TV and DVR regression.** The bind is system-wide, so broadcast playback
   must be checked before this ships, not only file playback.
3. **HDMV/BD paths.** `WEBOS25-DTS.md` records that which DTS recognition site a
   `.ts` lands in is decided entirely by its PMT; the 1.14 matrix has not been
   compared against the 1.24 one used for the C5 samples.

## Firmware provenance for the dependency check

Reproducing the check above, since picking the wrong image is easy — LG reuses
version numbers across boards, and a `23.25.55` in the JP mirror is a **W23H**
(Realtek B3) build, not this one.

- Index: `https://lg.slada.sk/processed_fw.json` (3 MB). Match on
  `firmwareotaID == HE_DTV_W22O_AFABATPU`, **not** on the version string.
- Package: `mirror2/UK/ruM3H7hNU8VG0Tc3ChteHA---Software_File(Version_23.25.55).zip`,
  1,509,767,293 bytes, sha256
  `9a732d00cc3d0ef0021f7f11b703ecb7acc6ec7e7b6f4b9bafca20e783a158ef` (matches the
  archive record). EPK `lib32-starfish-global-secured-o22-okapi.pine-61-23.25.55_prodkey_usb_V3_SECURED.epk`,
  board `o22`, platform `9.2.2`, firmware `23.25.55.01`.
- Download host is **`tv.slada.sk`**, not the `lg.slada.sk` SPA, and the link is
  built client-side so it does not appear in the served HTML. `curl` needs
  `--globoff` because the filename contains parentheses. LG's own
  `gscs-b2c.lge.com/downloadFile?fileId=…` returns **403**.
- `epk2extract` must show `Trying AES Key` lines — 48 here. None means no keys
  loaded (see the memory note); `build.sh` installs them beside the binary.
- The rootfs is **zstd** squashfs, so it needs a zstd-capable `unsquashfs`
  (Homebrew `squashfs`, 4.7.5 here). `unsquashfs -l rootfs.pak` lists the tree
  without extracting 1.3 GB, which is all this check needs.

## Order of work

1. ~~Build the demuxer~~ — done, `webos25/restore/ts114-out/`.
2. ~~Confirm its dependencies exist on the target~~ — done from firmware, above.
3. ~~Ship it behind an optional bind~~ — done, app 2.7.12 / `webos25-2.17`, in
   `payload/c2-ts/`. It degrades to MP4/MKV if it cannot stage or trace.
4. **Open:** confirm it loads on a real C2 (`gst-inspect-1.0 tsdemux`) and that the
   two bundled 5.1 DTS-HD MA samples play.
5. **Open:** check live TV and a DVR recording still play. The bind is system-wide,
   so this is the regression that matters and it is untested.
