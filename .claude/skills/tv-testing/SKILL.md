---
name: tv-testing
description: "Use when running anything on the rooted test TV (ssh webos-tv): installing a rebuilt plugin, boot script, kernel module or app build; playing DTS/DTS-HD/TrueHD audio; checking eARC, the route log or the kernel module; driving the DTS Enabler app; or verifying a change on-device before a release."
---

# Testing on the TV

The test TV: LG OLED42C27LA (C2), webOS 10.3.1, firmware 33.31.68, kernel
5.4.268-329.ptl4tv.5, rooted (Homebrew Channel). A Samsung soundbar is on eARC.

## Access

- `ssh webos-tv` (root, key auth; host 192.168.178.31). Use `-o BatchMode=yes`.
  Busybox userland: `ps -ef`, `scp -O` (legacy protocol).
- If it stops answering, the TV is in standby or rebooting: ping, wait, and ask the
  user to turn it on. After a reboot `/tmp` and `/run/dts25` are empty.

## Install a build

| What | Where on the TV |
|---|---|
| `restore/out/libgstdtsdec.so` | `cat` over `/var/lib/webosbrew/dts25/libgstdtsdec.so` (in place keeps the bind mount). Backup: `libgstdtsdec.so.orig` |
| `restore/init_dts25.sh` | `/var/lib/webosbrew/dts25/init_dts25.sh` (the boot hook `/var/lib/webosbrew/init.d/restore_dts25` runs it) |
| `kmod-out/*.ko`, `kmod/earc-route-load.sh` | `/var/lib/webosbrew/dts25/hdmi/` |
| app `index.html`, `js/app.js` | `/media/developer/apps/usr/palm/applications/io.github.josippapez.dtsenabler/` |
| app `service.js` | `/media/developer/apps/usr/palm/services/io.github.josippapez.dtsenabler.service/`, then `pkill -f io.github.josippapez.dtsenabler.service` |

Copy with `scp -O FILE webos-tv:/tmp/x`, then `cat /tmp/x > TARGET`. Whole-product
install is `sh install.sh` from the CLI tarball, or Enable in the app.

## Boot script modes

`sh /var/lib/webosbrew/dts25/init_dts25.sh` with: `W25_PT=start|stop` (passthrough
watcher and the earc_route holder), `W25_APPDTS=on|off`, `W25_CHECK=1` (report only),
`W25_STAND_DOWN=1`. Flags: `/var/lib/webosbrew/dts25/passthrough` and `passthrough-hd`.
Run modes with `</dev/null`, detached ones with `setsid`.

## Play audio

1. Tunnel the app's inspector: `ssh -N -L 19998:127.0.0.1:9998 webos-tv` (async).
   Kill a stale one first: `lsof -nP -iTCP:19998 -sTCP:LISTEN`.
2. Launch the app: `luna-send -n 1 luna://com.webos.applicationManager/launch '{"id":"io.github.josippapez.dtsenabler"}'`.
3. Click a sample with [cdp.py](cdp.py) (needs `websocket-client`):
   `python3 cdp.py 'document.getElementById("btnPlayTs").click()'`.
   Buttons: `btnPlayTs` (DTS-HD MA 5.1 .ts), `btnPlayTrueHd`. Read UI state the same way
   (`document.getElementById("sHd").textContent`). After replacing `index.html`, run
   `location.reload(true)`.

## Check the result

- Route log: `/run/dts25/lgroute.log` (truncate with `: >` before a play). HD working:
  `earc_route holds LG's bypass` then `eARC unmixed, carrier passes`.
- eARC: `grep -E 'OutputCodec|EArcOnOff' /proc/lgtv-driver/audio/sndout`;
  `Pass-Through` = bypass, `PCM`/codec name = mixed.
- Module: `lsmod | grep earc`, `/sys/module/earc_route/parameters/*`
  (`app_overrides`, `overrides`), `dmesg | grep earc_route:`, loader output
  `/run/dts25/hdmi-watch.out`, holder pid `/run/dts25/hdmi.pid`.
- Sound output: `/run/dts25/sound-output` (`<pid> external_arc passThrough`). HD needs
  Digital Sound Output on Pass Through.
- Boot script and app action log: `/tmp/dts25.log`.

## Rules

- Verify every rebuilt binary here before committing it (`.claude/rules/releasing.md`).
- Test the negative path too (module unloaded, HD off) and leave the TV as found:
  HD on, module loaded, stock backups untouched.
- Never `rmmod earc_route` while a stream plays; stop it through `W25_PT=stop`.
