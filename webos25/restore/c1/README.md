# `webos6-o20n-gst116-spdif` — DTS to an optical receiver on LG OLED C1/G1

**Status: hardware-verified on one TV (2026-11-17), still young.**
OLED65C17LB (`HE_DTV_W21O_AFABATAA`, firmware 03.53.45, webOS 6.5.3,
GStreamer 1.16.2), Pioneer VSX-915 on optical, Digital Sound Output = Pass
Through. Payload `libgstdtsdec.so` sha256
`dc5463404b0848cc5152d0a331a2ff964ebebc3ec9ad366c7d36c71fca486eae`.

| Check | Result |
|---|---|
| `tv-test.sh` (load + bit-exact IEC 61937 with LG's matroskademux on the TV CPU) | PASS (sha `5cdce24f…`, 712704 bytes) |
| Install, reboot, boot hook re-applies | PASS (`bound=yes`, watcher live) |
| LG Media Player, DTS 5.1 MKV from USB | receiver shows DTS |
| Moonfin 3.1.0 → Emby, DTS 5.1 MKV | Emby: Direct Play; LG media process maps `libgstdtsdec.so`; receiver DTS (STANDARD mode: large DTS indicator) |
| Native Emby app (1.0.51) | gates DTS by model year (transcodes); works with a server-side proxy that adds DTS to its MKV direct-play profile (kept out of this repo) |
| Kodi (webOS port) | not usable: with `tv.model.edidType` +dts Kodi's media pipeline fails with `INT_ERROR 601 Resource Allocation Error` (LG resource manager has no DTS decoder resource); keep edidType stock (`TrueHD`) and Kodi decodes DTS to PCM itself |
| `tv-test.sh listen` (gst-launch straight to `audiosink`) | hangs outside LG's media service (expected; not a plugin issue) |

Not tested: DTS in MP4/TS/M2TS, HDMI ARC, 44.1/96 kHz DTS cores (decoded to stereo by design).

Isolated from every other profile: its own state (`/var/lib/webosbrew/dtsc1`),
its own boot hook (`init.d/restore_dts_c1`), and it refuses to run next to the
CX (`restore_dts`), C2 or webOS 25 hooks, or any demuxer/libav override. The
existing app profile `webos6-armel-gst116` (legacy CX payload) is unchanged.

## What it does

The C1's Matroska demuxer already emits `audio/x-dts`; there is simply no
decoder. This injects one `dtsdec` (`../src/gst116/gstdtsdec116.c`, built by
`../build-gst116.sh`) that, per stream:

- **passthrough**: wraps each DTS core frame in IEC 61937 and outputs it as
  plain S16LE 48 kHz stereo, byte-identical to FFmpeg's `spdif` muxer. The TV
  forwards it untouched; the receiver decodes DTS 5.1. Only when opted in
  **and** the live sound output is `external_optical`/`external_arc` **and**
  Digital Sound Output is `passThrough`, re-checked about once a second. DTS-HD
  extensions are dropped: S/PDIF carries the core.
- **decode** otherwise: FFmpeg `dca` → stereo S16LE, so TV speakers never get
  the bitstream (which would be loud noise).

Other formats (AAC, AC-3, E-AC-3, PCM) never reach it. `device_codec_capability_config.json`
on the C1 already lists `DTS`/`DTSH`/`DTSE`, so it is not touched.

## How it is applied (and undone)

A new plugin registry (LG's plugins + `dtsdec`) is generated in `/run`, proven
(nothing of LG's may go missing; `dtsdec` must load), and **bind-mounted
read-only** over LG's media registry. LG's file is never written: `umount`, a
reboot without the hook, or `uninstall-c1.sh` restores it exactly.

## Order of testing

1. `sh tv-test.sh` — loads the plugin from `/tmp` with a private registry and
   checks bit-exact output on the TV's CPU. Changes nothing.
2. `sh tv-test.sh listen` — receiver volume down; tries LG's `audiosink`
   directly. May not open outside LG's media service; then go to 3.
3. `sh install-c1.sh --passthrough` — then a new playback in the Media Player
   (MKV with DTS). Check `sh /var/lib/webosbrew/dtsc1/init_dts_c1.sh status`
   (`last_decision`).
4. `sh uninstall-c1.sh` to remove everything.

## Apps

- **LG Media Player, Moonfin (≥ 2.0.1, webOS 6 enables DTS in MKV):** work as-is.
- **Native Emby app:** decides DTS support from the model year (2020–2022 = no DTS;
  its JS is NCG-encrypted, not patched), so the server transcodes. A small reverse
  proxy in front of the Emby server that adds `dts` to the app's MKV direct-play
  profile in `PlaybackInfo` makes it direct-play; it lives outside this repo.
  Image subtitles (PGS) force a transcode anyway (`SubtitleCodecNotSupported`).
- **Kodi:** see the table above.
- `device_codec_capability_config.json` on the C1 already lists `DTS`/`DTSH`/`DTSE`; not touched.

## Known quirks fixed during bring-up

- Old busybox on webOS 6 only knows `timeout -t SECS`; the init script detects both forms.
- `/mnt/flash/data` is reached through a symlink, so mount checks resolve the path.
- GstAudioDecoder's default sink getcaps proxied `channels=2` onto the sink caps
  and refused 5.1 tracks; `dtsdec` answers its template.
- The decode path unmapped the input after `finish_frame()` released it; fixed.
- `last_decision` stays `none`: LG's media process is not root and cannot write
  `/run/dtsc1` (cosmetic; the receiver display is the check).

## Build / verification

`../build-gst116.sh` (Docker; Ubuntu 18.04 armel cross, glibc 2.27 sysroot;
glib 2.58.3, GStreamer 1.16.2, FFmpeg n4.4.4 from source at pinned commits).
It checks e_flags `0x5000200`, no RPATH, max `GLIBC_2.17` (TV: 2.28), zero
exported FFmpeg symbols, then runs qemu tests on the real 1.16.2 core:
bit-exact passthrough via qtdemux, matroskademux and raw ES; decode fallback;
the six auto-mode cases; decodebin autoplug. See `../gst116-out/BUILD-REPORT.txt`.
