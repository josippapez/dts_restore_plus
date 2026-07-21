# DTS Enabler

A webOS Homebrew app that turns the root-shell `dts_restore` install into a
friendly, remote-navigable TV app. Enable/disable DTS restore, view status,
tune the stereo downmix coefficients, and toggle Dolby Vision — no SSH, no
hand-editing files.

> Wraps [`dts_restore`](https://github.com/lgstreamer/dts_restore) by Pete
> Batard. Designed for **rooted** LG TVs (OLED CX-class, webOS 5.x, GStreamer
> 1.14.4). Not endorsed by LG.

> **STATUS: UNTESTED.** This app was scaffolded without access to a TV or the
> webOS SDK. Nothing here has been built, packaged, or run on hardware. See
> [What is untested](#what-is-untested).

---

## Architecture

```
┌────────────────────────────────────────────────────────────┐
│  DTS Enabler web app  (org.webosbrew.dtsenabler)            │
│  index.html + js/app.js + css/style.css                     │
│  - D-pad navigable UI, validates downmix inputs client-side │
│         │ webOS.service.request / PalmServiceBridge         │
│         ▼                                                    │
│  JS service  (org.webosbrew.dtsenabler.service)             │
│  service/service.js                                         │
│  - builds sanitised shell commands                          │
│  - re-validates all inputs (defence in depth)              │
│         │ luna://org.webosbrew.hbchannel.service/exec        │
│         ▼                                                    │
│  Homebrew Channel exec service  (runs as ROOT)             │
│         ▼                                                    │
│  bind-mounts .so overrides, rewrites gstcool.conf,          │
│  installs /var/lib/webosbrew/init.d/restore_dts,            │
│  manages /tmp/dv_disable                                    │
└────────────────────────────────────────────────────────────┘
```

The app holds **no elevation of its own**. Every privileged action is a shell
string sent to the Homebrew Channel (HBC) exec service, which is the
recommended root path for rooted-TV homebrew (same model as `magic_mapper`).

### What it manages (mirrors dts_restore)

| Concern | On-device effect |
| --- | --- |
| Enable DTS | Stage `.so` payload to `/var/lib/webosbrew/dtsenabler/gst`, bind-mount over `/usr/lib/gstreamer-1.0/`, rebuild GStreamer registry, rewrite `gstcool.conf` (`avdec_dca=0`→`290` + `[downmix]`), install `init.d/restore_dts` for boot persistence. Applied immediately, no reboot. |
| Disable DTS | Remove `init.d/restore_dts`, best-effort unmount. Original LG files are never modified, so it is fully reversible. |
| Status | `nyx-cmd` model/webOS, `gst-inspect-1.0` version, `grep gstreamer-1.0 /proc/mounts`, `avdec_dca` rank, init-script presence, DV flag. |
| Downmix | 5 coefficients (front/center/lfe/rear/rear2) persisted to `config.json`, regenerate + re-bind `gstcool.conf`. |
| Dolby Vision | Create/remove `/tmp/dv_disable`; persist so the init script recreates it at boot. |

### Persistent state

Everything the app owns lives under `/var/lib/webosbrew/dtsenabler/`:
`gst/` (staged `.so` copy), `config.json` (downmix + DV), `dv_disabled` (boot
flag). The init script reads from these stable paths so it works regardless of
where/when the app partition mounts.

---

## Security model (read this)

**The exec service runs the commands we send it as root.** Every value that
reaches a command string is treated as hostile.

- The only caller-controlled inputs are the **5 downmix coefficients** and a
  **DV boolean**.
- Coefficients pass through `validateCoeff()` (service side) **and**
  `normaliseCoeff()` (client side): coerced with `Number()`, range-checked to
  `[0.0, 2.0]`, and emitted as a fixed `"d.dd"` string matched against
  `/^[0-9]+\.[0-9]{2}$/`. A value containing anything shell-meaningful
  (`;`, `$`, backticks, spaces, `../`, …) fails the numeric coercion or the
  regex and is rejected before it can reach a shell.
- The DV flag is coerced to a JS boolean and only ever emitted as literal
  command branches — never interpolated.
- All paths in commands are **constants** defined in `service.js`; no untrusted
  path fragment is ever interpolated.
- Config and the generated init script are written via `base64 -d` heredocs, so
  no content survives the write as shell syntax.

Client-side validation is UX; **server-side validation in `service.js` is the
real boundary** — never remove it.

---

## Repository layout

```
dts-enabler-app/
├── appinfo.json          # app metadata (id, title, icon, permissions)
├── icon.svg              # placeholder launcher icon (convert to icon.png)
├── index.html            # UI
├── css/style.css         # TV-remote styling + focus ring
├── js/app.js             # controller: service calls, D-pad nav, validation
├── service/
│   ├── package.json      # JS service manifest
│   ├── services.json     # Luna service + method registration
│   └── service.js        # privileged backend (command construction + exec)
├── vendor/
│   ├── README.md         # how to vendor the dts_restore .so payload
│   └── gst/              # <- drop the .so files here before packaging
├── .gitignore
└── README.md
```

---

## Icon

`appinfo.json` references `icon.png`, but this scaffold ships a vector
placeholder (`icon.svg`) because a binary PNG can't be generated here. Convert
before packaging:

```sh
rsvg-convert -w 80 -h 80 icon.svg > icon.png
# largeIcon (optional but recommended):
rsvg-convert -w 130 -h 130 icon.svg > largeIcon.png   # then point appinfo at it
```

---

## Build / package

Prerequisites: the [webOS OSE / TV CLI](https://github.com/webosose/ares-cli)
(`npm i -g @webosose/ares-cli`) or webosbrew dev tooling, plus the vendored
`.so` payload (see `vendor/README.md`).

```sh
# 0. Vendor the payload
cp /path/to/dts_restore/gst/*.so vendor/gst/

# 1. Generate the icon (see above)
rsvg-convert -w 80 -h 80 icon.svg > icon.png

# 2. Package app + service into one .ipk
#    (-s bundles the JS service directory alongside the app)
ares-package . service

# -> org.webosbrew.dtsenabler_1.0.0_all.ipk
```

For submission to `repo.webosbrew.org`, follow the webosbrew
[submission guide](https://www.webosbrew.org/pages/develop.html) (a
`manifest.json` + PR to the apps repo). The Homebrew Channel then lists it and
handles auto-updates.

---

## Install

**Via Homebrew Channel (end users):** once listed, find "DTS Enabler" in HBC and
install; updates are automatic.

**Sideload (development), TV in dev mode + rooted:**

```sh
ares-setup-device                       # register your TV once
ares-install ./org.webosbrew.dtsenabler_1.0.0_all.ipk
ares-launch org.webosbrew.dtsenabler
```

The Homebrew Channel must be installed on the TV, because this app calls its
exec service for all root work.

---

## Test on a real rooted TV (must be validated on-device)

1. **Prereqs:** rooted TV, Homebrew Channel installed, `dts_restore` `.so`
   payload vendored, media player idle (gstcool.conf can't be re-bound while
   `starfish-media` runs).
2. Launch the app; confirm the **Status** panel populates (model/webOS/GStreamer,
   mounts, rank). If it shows "unavailable", the exec service isn't reachable
   (HBC missing or not rooted).
3. **Enable DTS** → verify `avdec_dca rank` becomes `290`, "Overrides mounted"
   = yes, "Init script" = installed. Check `/tmp/dtsenabler.log`. Play a DTS
   file and confirm audio.
4. **Downmix** → move sliders, Apply; confirm `/tmp/gstcool.conf` `[downmix]`
   matches and audio balance changes (media player must be idle for the re-bind).
5. **DV toggle** → Disable, confirm `/tmp/dv_disable` exists and hybrid DV+HDR
   plays as HDR; Enable removes it.
6. **Reboot** → confirm the init script re-applies everything (mounts, rank,
   persisted downmix, DV choice).
7. **Disable DTS** → confirm init script removed and, after reboot, playback is
   back to stock.

---

## What is untested

**Everything.** No TV, no ares SDK, no build was available. In particular,
verify on-device:

- The exact **HBC exec service name/params** (`.../exec` `{command}`) and its
  response shape (`stdout`/`stderr`/`returnValue`); adjust `rootExec()` if your
  HBC build differs (there is also a `spawn` variant).
- That `webos-service` is the correct require for the JS service on your target
  firmware, and that `services.json`/`package.json` register the bus name the
  frontend calls.
- Whether re-binding `gstcool.conf` live (without a reboot) actually takes
  effect for the running media pipeline, or requires the player to restart.
- The `requiredPermissions` set in `appinfo.json` (ACG groups) for your
  firmware.
- Idempotency of the mount/unmount guards across enable → disable → enable.
- Registry override behaviour (`ORIG_REG` capture) on your GStreamer build.
- That the vendored `.so` set matches your TV model / GStreamer 1.14.4.

## License

App code: LGPL-2.1-or-later (matching the GStreamer plugins it manages). The
vendored `.so` payload is LGPL-2.1+ from upstream dts_restore — ship the
corresponding source offer with any distributed `.ipk`.
