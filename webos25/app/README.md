# DTS Enabler (universal)

One webOS Homebrew app that restores DTS audio on rooted LG TVs **across
generations**. It detects the TV at runtime, picks the correct DTS-restore
mechanism for that generation, and offers **Enable / Disable / Uninstall** plus
a live **status panel** — no SSH, no hand-editing files, no guessing which
binary set to use.

This app **supersedes** the single-target `dts-enabler-app/` (which only knew
the CX demuxer-override mechanism and would have been actively harmful on a
webOS-25 TV — see MULTI-MODEL.md §3.1).

> Wraps the on-device work in `../webos25/` (verified webOS-25 mechanism) and
> [`dts_restore`](https://github.com/lgstreamer/dts_restore) by Pete Batard (CX
> mechanism). Requires a **rooted** LG TV with the **Homebrew Channel**
> installed. Not endorsed by LG.

---

## The detect → branch model

The app never assumes a mechanism. On launch (and on every **Refresh**) the JS
service runs a **read-only** detection probe (the logic of
`../webos25/detect-target.sh`, embedded verbatim as a constant) that measures the
three axes that decide binary compatibility and fix strategy:

1. **CPU arch / float ABI of the GStreamer userspace** — dynamic loader name +
   the ELF `e_flags` float-ABI nibble of a real `/usr/lib/gstreamer-1.0/*.so`
   (not `uname -m`: the C5 has an aarch64 kernel but a 32-bit soft-float
   userspace).
2. **GStreamer version** — `gst-inspect-1.0 --version` (drives plugin ABI).
3. **How LG disabled DTS** — which decoders are registered, and a static
   heuristic on `libgstmatroska.so` (A_DTS re-tag vs. demux nerf).

Those collapse to a **profile**, and the profile selects the mechanism:

```
                       ┌─────────────────────────┐
        detect() ─────▶│  read-only probe (5×)    │──▶ PROFILE=…
                       └─────────────────────────┘
                                   │
        ┌──────────────────────────┼───────────────────────────┐
        ▼                          ▼                            ▼
 webos25-armel-gst124       cx-armv7-gst114              unknown / unknown-*
 (GStreamer 1.24,           (GStreamer 1.14)             (anything else)
  ld-linux.so.3, soft)            │                            │
        │                         │                            ▼
        ▼                         ▼                       REFUSE cleanly
 decoder-inject            demuxer-override            (never apply a
 (patched dtsdec)          (rebuilt LG libs +           mismatched mechanism)
                           avdec_dca rank bump)
```

`enable` / `disable` / `uninstall` each **re-detect** (they never trust the
client) and dispatch to the matching mechanism's hardcoded command builder. An
unknown/unsupported profile is refused with a clear message — we never apply an
ABI-mismatched mechanism (CX 1.14 libs on a 1.24 TV would break MKV/MP4).

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  DTS Enabler web app  (io.github.josippapez.dtsenabler)             │
│  index.html + js/app.js + css/style.css                      │
│  - D-pad navigable UI, status panel, Enable/Disable/Uninstall│
│  - sends NO free-form parameters (methods take none)         │
│         │ webOS.service.request / PalmServiceBridge          │
│         ▼                                                     │
│  JS service  (io.github.josippapez.dtsenabler.service)             │
│  service/service.js                                          │
│  - detects the profile (read-only probe)                    │
│  - branches to the matched mechanism's command builder      │
│  - all shell is author constants (empty injection surface)  │
│         │ luna://org.webosbrew.hbchannel.service/exec         │
│         ▼                                                     │
│  Homebrew Channel exec service  (runs as ROOT)              │
│         ▼                                                     │
│  webOS 25: stage dtsdec+libdca, regen+bind media registry   │
│  CX:       bind-mount demuxer libs, bump avdec_dca, gstcool │
└──────────────────────────────────────────────────────────────┘
```

The app holds **no elevation of its own**. Every privileged action is a shell
string sent to the Homebrew Channel (HBC) exec service via the hardened
`rootExec()` wrapper carried over from the single-target app.

---

## Per-profile mechanism

### `webos25-armel-gst124` — decoder-inject + demux-enable (VERIFIED)

Mirrors `../webos25/restore/install.sh` + `init_dts25.sh` exactly. Restores **DTS
(incl. mp4/ts/m2ts containers) and TrueHD/MLP**.

- **Enable:** stage three payloads — DTS (`libgstdtsdec.so` + `libdca.so.0` →
  `/var/lib/webosbrew/dts25/`), TrueHD (`libgstlibav.so` + ffmpeg libs →
  `/var/lib/webosbrew/truehd/`), and the container demuxers (patched
  `libgstisomp4.so` + `libgstmpegtsdemux.so`, `dts_support` default TRUE →
  `/var/lib/webosbrew/demux25/`). Generate the two `/etc` overrides
  (codec-capability TRUEHD/MLP; gstcool `avdec_truehd/mlp=310`). Write the
  canonical `init_dts25.sh`, which bind-mounts our libav, the demuxers, and the
  overrides, then regenerates the media GStreamer registry and writes it to
  `/mnt/flash/data/gst_1_0_registry.arm.bin`. Symlink the boot hook, apply now,
  restart `starfish-media-pipeline`.
- **Disable:** remove the boot hook, unmount every bind (libav, demuxers, both
  `/etc` overrides, registry) → LG originals restored. Staged libs kept.
- **Uninstall:** disable + `rm -rf /var/lib/webosbrew/{dts25,truehd,demux25}`.
- **Test (self-check):** the `test` method decodes a bundled DTS sample per
  container (mp4/ts/m2ts) through the media registry and returns PASS/FAIL — an
  objective "is the patch working" check independent of the speaker/output stage.
  The UI also offers **play-by-ear** of the bundled samples (in-app `<video>`).

### `cx-armv7-gst114` — demuxer-override (UNVERIFIED)

Mirrors the repo-root `install.sh` / `init_dts.sh`. CX-era firmware strips the
DTS pad from the demuxer, so the fix is library-override-centric.

- **Enable:** stage the rebuilt LG `.so` set → `/var/lib/webosbrew/dtsenabler/cx/gst/`;
  bake the GStreamer registry path from the exec session into `env.conf`; write
  `init_dts.sh`; install the boot hook `/var/lib/webosbrew/init.d/restore_dts`;
  apply now. The init script bind-mounts (read-only) the demuxer/libav libs over
  `/usr/lib/gstreamer-1.0/`, refreshes the registry, and bumps `avdec_dca`
  `0→290` with a `[downmix]` section in `gstcool.conf`.
- **Disable:** remove the boot hook, best-effort unmount all overrides +
  gstcool + registry bind. LG's own files were never modified.
- **Uninstall:** disable + `rm -rf /var/lib/webosbrew/dtsenabler/cx`.

---

## Security model

### Exec-bridge permissions (why the service can reach hbchannel)

hbchannel's `/exec` is already a **public** method that accepts any caller (once the
TV is rooted, hbchannel rewrites the device LS2 config to `allowedNames:["*"]` /
`inbound:["*"]`). The gate is on **our** side: a JS service's default role does not
grant **outbound** access, so its call to hbchannel is rejected. We therefore ship
three ACG manifest files next to `services.json` (discovered by naming convention;
appinstalld builds the `manifests.d` entry from them):

- `io.github.josippapez.dtsenabler.service.role.json` — `permissions[].outbound:["*"]`
  (the load-bearing line — lets us call `org.webosbrew.hbchannel.service`),
  `inbound:["*"]`, `allowedNames`.
- `io.github.josippapez.dtsenabler.service.api.json` — declares our own methods `public`.
- `io.github.josippapez.dtsenabler.service.perm.json` — client/outbound ACG grant.

**Guaranteed fallback (rooted TV):** if a given firmware doesn't auto-discover them,
copy the three files into the live LS2 dev config and restart the hub once:

```sh
cp io.github.josippapez.dtsenabler.service.role.json /var/luna-service2-dev/roles.d/
cp io.github.josippapez.dtsenabler.service.api.json  /var/luna-service2-dev/api-permissions.d/
cp io.github.josippapez.dtsenabler.service.perm.json /var/luna-service2-dev/client-permissions.d/
ls-control scan-services 2>/dev/null || killall -HUP ls-hubd 2>/dev/null || reboot
```

This is exactly the mechanism hbchannel uses for itself
(`webos-homebrew-channel/services/elevate-service.ts`). **Status: shipped in the
service dir; on-device auto-discovery vs. the fallback is still to be confirmed.**

### Security model

- **Everything handed to the exec service runs as root.**
- This app takes **no caller-controlled shell input**: no method has a free-form
  parameter. Every path, filename, rank, and downmix coefficient is a hardcoded
  constant in `service.js`, validated at author time. The injection surface is
  empty by construction (the single-target app's `validateCoeff`/`normaliseCoeff`
  guards existed for its downmix sliders, which this app deliberately drops).
- The **detected profile** is the only value that steers behaviour, and it is
  matched against a fixed allowlist (`PROFILE_W25` / `PROFILE_CX`) before any
  mechanism runs. An unrecognised profile is **refused**, never interpolated.
- Generated init scripts are written via `base64 -d` heredocs, so no content
  survives the write as shell syntax.
- The detection probe is strictly **read-only** — it inspects, it never mounts,
  copies, or modifies anything.

---

## Repository layout

```
dts-enabler-universal/
├── appinfo.json              # app metadata (id io.github.josippapez.dtsenabler)
├── icon.svg                  # placeholder launcher icon (convert to icon.png)
├── index.html                # status panel + Enable/Disable/Uninstall UI
├── css/style.css             # TV-remote styling + focus ring
├── js/app.js                 # controller: detect/status/enable/disable/uninstall, D-pad nav
├── service/
│   ├── package.json          # JS service manifest
│   ├── services.json         # Luna service + method registration
│   └── service.js            # detect + per-profile mechanism builders + exec
├── payload/
│   ├── webos25/              # <- drop libgstdtsdec.so + libdca.so.0 (see README)
│   │   ├── .gitkeep
│   │   └── README
│   └── cx/                   # <- drop CX demuxer/libav .so set (see README)
│       ├── .gitkeep
│       └── README
├── .gitignore
└── README.md
```

---

## Build / package

Prereqs: the [webOS CLI](https://github.com/webosose/ares-cli)
(`npm i -g @webosose/ares-cli`), plus the vendored payload for each profile you
want to support.

```sh
# 1. Populate the payloads (see payload/*/README for provenance)
cp ../restore/out/libgstdtsdec.so ../restore/out/libdca.so.0   payload/webos25/
cp ../restore/truehd-out/libgstlibav.so ../restore/truehd-out/libav*.so* \
   ../restore/truehd-out/libsw*.so*                            payload/webos25-truehd/
cp ../restore/demux-out/libgstisomp4.so ../restore/demux-out/libgstmpegtsdemux.so \
                                                               payload/webos25-demux/
# small DTS samples for the self-test / play-by-ear (already bundled)
# payload/testfiles/{DTS-in-mp4.mp4,DTS-HD-MA-5.1.ts,DTS-HD-MA-5.1.m2ts}
cp ../gst/*.so                                                 payload/cx/  # CX only

# 2. Generate the icon
rsvg-convert -w 80 -h 80 icon.svg > icon.png

# 3. Package app + service into one .ipk (-s bundles the JS service dir)
ares-package . service
# -> io.github.josippapez.dtsenabler_2.0.0_all.ipk
```

You can ship an `.ipk` with only one payload populated (e.g. webOS-25 only): the
service just refuses to enable on a profile whose payload is absent, logging a
clear WARN to `/tmp/dtsenabler.log`.

## Install

**Via Homebrew Channel — add our repository (works now, everyone):**
In the Homebrew Channel go to **Settings → Add repository** and paste:

```
https://josippapez.github.io/dts_restore_plus/api/apps.json
```

"DTS Enabler" then appears in the app list and installs — the `.ipk` is pulled from
the GitHub release and its **sha256 is verified** against the hosted manifest
(`https://github.com/josippapez/dts_restore_plus/releases/latest/download/io.github.josippapez.dtsenabler.manifest.json`).
Updates flow automatically as new releases are tagged (the repo is regenerated by CI).

*(Or, once accepted into the official [`webosbrew/apps-repo`](https://github.com/webosbrew/apps-repo), it appears in HBC by default with no custom repo — see `packaging/homebrew/`.)*

**Sideload (development), TV in dev mode + rooted:**

```sh
ares-setup-device
ares-install ./io.github.josippapez.dtsenabler_2.0.0_all.ipk
ares-launch io.github.josippapez.dtsenabler
```

The Homebrew Channel must be installed on the TV — the app calls its exec
service for all root work.

---

## STATUS (honest)

| Profile | TV family | Mechanism | Status |
|---|---|---|---|
| `webos25-armel-gst124` | LG C5 / G5 (webOS 25, GStreamer 1.24, armel soft-float) | decoder-inject (patched dtsdec + libdca) + TrueHD (avdec_truehd) | **Mechanism VERIFIED playing on a real C5** (via the `restore/` CLI install): both DTS and TrueHD decode and play, LG's sink receives `audio/x-raw, S32LE` (5.1), persistent across reboot. NOTE: the exec-bridge **role/permission manifest is now shipped** (service `*.role.json` with `outbound:["*"]` + api/perm files — see "Exec-bridge permissions"), so the app's detect/enable/test should reach the Homebrew Channel; **on-device confirmation (auto-discovery vs. the `/var/luna-service2-dev/` fallback) is pending**. The `restore/install.sh` CLI path remains the verified route. The app now also stages the **container demuxers** and includes a **self-test + play-by-ear** for mp4/ts/m2ts. |
| `cx-armv7-gst114` | OLED CX / BX / C1 / C2 / NanoCell (webOS 3–6, GStreamer 1.14) | demuxer-override (rebuilt LG libs + `avdec_dca` rank) | **Carried over, UNVERIFIED by this project** — no CX hardware. Mechanism is the field-used shipping `dts_restore` recipe, but its "armv7 hard-float" ABI claim was never measured on-device (the C5 proved the same triplet can be soft-float). Confirm the loader/e_flags via `detect` before trusting. |
| webOS 22 / 23 / 24 (C2/C3/C4/G-series) | — | none | **No recipe.** Arch/ABI/GStreamer/disable-mechanism all unknown; the detector emits an `unknown-*` profile and the app **refuses**. |
| anything else | — | none | Detector emits `unknown-*`; app refuses. |

Open questions: DTS decodes to **S32LE, up to 5.1**, and TrueHD to **S32LE** —
LG's integer-only sink accepts these (confirmed on-device; the earlier F32LE issue
was fixed by converting dtsdec's output to S32LE). The remaining unknown is whether
the TV **renders full surround** to speakers/eARC or downmixes to stereo (not
independently measured). Bitstream **passthrough** to an AVR is out of scope
(months of proprietary-lib RE). See `../docs/MULTI-MODEL.md` for the full gap list.

## License

App code: LGPL-2.1-or-later. The vendored `.so` payloads are LGPL-2.1+ release
artifacts (not committed here) — ship the corresponding source offer with any
distributed `.ipk`.
