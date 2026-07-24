# Make-up gain: tuning + rebuild/release runbook

Background and mechanism: [`../docs/WEBOS25-DTS.md#loudness--make-up-gain`](../docs/WEBOS25-DTS.md#loudness--make-up-gain).
This file is the step-by-step procedure for (1) tuning the gain by ear from the
app, (2) hand-editing the config files directly, and (3) the binary rebuild →
on-device-verify → recommit → tag loop required whenever the gain code itself
changes.

## 1. Tuning from the app (no SSH, no rebuild)

1. Open **DTS Enabler** → the **Make-up gain** card (see
   [`../app/README.md#make-up-gain-control`](../app/README.md#make-up-gain-control)).
2. Play a DTS or TrueHD clip you know well, and its native-codec equivalent
   (AAC/AC-3/Atmos) at the same source loudness, back to back.
3. Start at **0 dB** (unity — today's behavior) on the relevant field.
4. Raise in **~2 dB steps**, replaying the DTS/TrueHD clip after each step,
   until it sounds roughly as loud as the native reference.
5. **Back off at the first sign of clipping or distortion.** Both decoders
   apply the gain in float and then hard-clamp to the S32 range
   (`gstdtsdec.c:766-777`; the TrueHD patch's `mlp_apply_makeup_gain`,
   `build-truehd.sh:172-199`) — the clamp is **silent**: there is no
   log/error/dropout, just flattened peaks. If it sounds off, drop back one
   step (~2 dB) and stay there.
6. Hit **Save gain**. It applies on the **next** playback of that codec — no
   reboot, no re-detect.
7. Tune DTS and TrueHD independently; they're separate fields writing
   separate files.

## 2. Hand-editing the config files (SSH, no rebuild)

For users who prefer editing directly instead of the app:

| Codec | Path |
|---|---|
| DTS | `/var/lib/webosbrew/dts25/gain.conf` |
| TrueHD/MLP | `/var/lib/webosbrew/truehd/gain.conf` |

Format: a single ASCII float = gain in dB (e.g. `6.0`). `#` comments and blank
lines ignored, whitespace tolerated. Missing/empty/unparseable → **0.0 dB
(unity, no-op)** — never fails decode. Parsed value is clamped to
**[-20.0, +20.0] dB** before use. Takes effect on the **next playback**
(read once at decoder init) — no reboot or registry re-init needed.

```sh
mkdir -p /var/lib/webosbrew/dts25 /var/lib/webosbrew/truehd
printf '%s\n' '6.0'  > /var/lib/webosbrew/dts25/gain.conf
printf '%s\n' '4.0'  > /var/lib/webosbrew/truehd/gain.conf
```

## 3. When you change the gain source itself: rebuild → verify → recommit → tag

This section only applies if you edit the **code** that implements make-up
gain — `webos25/restore/src/gstdtsdec.c` or `webos25/restore/build-truehd.sh` —
not when just changing a `gain.conf` value. Both files are **binary-affecting**
per [`../../.claude/rules/releasing.md`](../../.claude/rules/releasing.md):
the committed `.so` in `restore/{out,truehd-out}` are the source of truth and
must be kept in sync, on a real TV, in the same change.

1. **Rebuild both binaries:**
   ```sh
   ./build.sh          # -> out/libgstdtsdec.so, out/libdca.so.0
   ./build-truehd.sh   # -> truehd-out/libgstlibav.so + libav*/libsw* (applies the mlpdec gain patch)
   ```
2. **Check each build's `BUILD-REPORT.txt`** (`out/BUILD-REPORT.txt`,
   `truehd-out/BUILD-REPORT.txt`) — confirms the ELF is armel soft-float
   (`e_flags 0x05000200`) and every GLIBC symbol is `<= 2.35`, matching the C5
   userspace (see [`../README.md#target-abi-the-other-crux`](../README.md#target-abi-the-other-crux)).
   `build-truehd.sh` also self-verifies the patch applied
   (`grep -q mlp_apply_makeup_gain libavcodec/mlpdec.c`, `build-truehd.sh:252-257`)
   before it produces output.
3. **Deploy + on-device verify on a real webOS-25 TV** (rooted C5 or
   equivalent): copy the rebuilt `restore/` to the TV, `sh install.sh` (or
   re-run it if already installed), then:
   - Play a DTS clip and a TrueHD clip; A/B against a native AAC/AC-3 clip at
     comparable source loudness — confirm the make-up-gained output is
     audibly closer to native loudness and **not clipping**.
   - Confirm AAC/AC-3/Spotify playback (and any other codec through
     `libgstlibav.so`) is **unaffected** — this is the regression class the
     TrueHD patch is designed to avoid (codec-local change, see
     `TRUEHD-BUILD.md`), but on-device confirmation is still required since
     this environment cannot itself build ARM binaries or run on a TV.
4. **Recommit the rebuilt `.so`** in the **same change** as the source edit —
   `restore/out/`, `restore/truehd-out/` (and their `BUILD-REPORT.txt`).
   Never let the committed binaries drift behind the source.
5. **Tag the release:**
   ```sh
   git tag webos25-<X.Y>
   git push origin webos25-<X.Y>
   ```
   The **Release (webOS 25)** GitHub Action packages the CLI tarball + builds
   the `.ipk` from the tag — it does not rebuild from source, so step 4 must
   already be committed. Confirm the run is green and both assets are
   attached. Full procedure: [`../../RELEASING.md`](../../RELEASING.md).

## 4. Scratch builds are not part of this flow

The untracked, workspace-root `truehd-rebuild/` and `truehd-build2/`
directories (outside this git repo, alongside it on disk) are **ad hoc scratch
builds** from earlier iteration — they are not referenced by `install.sh`, not
wired into any build script here, and not part of CI or the release Action.
The only builds that matter for install/CI/release are `build.sh` and
`build-truehd.sh` in this directory, producing `out/` and `truehd-out/`.
