# Make-up gain + DRC: tuning + rebuild/release runbook

Background and mechanism: [`../docs/WEBOS25-DTS.md#loudness--make-up-gain`](../docs/WEBOS25-DTS.md#loudness--make-up-gain)
(make-up gain) and its "Dynamic range compression (DRC) + dialogue boost"
subsection (the DRC model + the LG evidence behind it). This file is the
step-by-step procedure for (1) tuning gain/DRC/dialogue-boost by ear from the
app, (2) hand-editing the config files directly, and (3) the binary rebuild →
test → on-device-verify → recommit → tag loop required whenever the DSP code
itself changes.

## 1. Tuning from the app (no SSH, no rebuild)

1. Open **DTS Enabler** → the **Make-up gain & dynamic range** card (see
   [`../app/README.md#make-up-gain--drc-control`](../app/README.md#make-up-gain--drc-control)).
2. Play a DTS or TrueHD clip you know well, and its native-codec equivalent
   (AAC/AC-3/Atmos) at the same source loudness, back to back.
3. **Gain first, DRC second — they solve different problems, and they stack.**
   Because DRC also lifts quiet passages toward the null band, **the more DRC
   is doing (Medium/Night vs. Light/Off), the less static make-up gain you
   typically need** to reach the same overall loudness — retune gain downward
   a step or two after raising the DRC preset, rather than leaving whatever
   gain you picked before turning DRC on:
   - **Gain** raises everything equally. Start at **0 dB** (unity), raise in
     **~2 dB steps**, replaying after each step, until *overall* loudness is
     roughly in line with the native reference.
   - **DRC preset** then fixes the *dialogue-vs-effects balance* gain alone
     cannot: cycle **Off → Light → Medium → Night** and judge whether quiet
     dialogue got easier to follow without loud passages getting harsher.
     Night is the heaviest (RF-style) profile — try it for late-night viewing
     first, Light/Medium for normal viewing. After picking a preset, re-check
     whether the gain you set in the previous step is still needed, or is now
     over-driving the loud passages.
   - **Dialogue boost** (the centre-channel stepper, dB) is an independent
     lift on top of DRC, for material where dialogue specifically still sits
     under effects/music after picking a preset. It has no effect on layouts
     without a discrete front-centre channel.
4. **Quick objective check — in-app A/B compare.** Before or after the by-ear
   pass above, press **Render A/B** on the same card: it renders the bundled
   DTS sample twice (DRC off/0 dB vs. your currently-saved settings, via
   `dtsdec` properties — `gain.conf` is never touched) and reports the
   measured dB delta between them, so you have a number confirming the
   preset is doing something even before — or in addition to — trusting
   your ears. It exercises the DTS decoder path only (the bundled sample is
   DTS) and does not replace the by-ear pass, which is still how you judge
   the *dialogue-vs-effects balance* rather than raw level. See
   [`../app/README.md#ab-compare-hear-the-drc-on-the-same-clip`](../app/README.md#ab-compare-hear-the-drc-on-the-same-clip).
5. **Back off at the first sign of clipping or distortion.** All three stages
   (make-up gain, DRC, centre boost) are applied in float and then hard-clamp
   to the S32 range (`gstdtsdec.c:1520-1538`; the TrueHD patch's equivalent
   per-sample loop, `build-truehd.sh`) — the clamp is **silent**: there is no
   log/error/dropout, just flattened peaks. If it sounds off, drop back one
   gain step or one DRC preset level and stay there.
6. Hit **Save audio settings**. It applies on the **next** playback of that
   codec — no reboot, no re-detect.
7. Tune DTS and TrueHD independently; they're separate fields writing
   separate files.

## 2. Hand-editing the config files (SSH, no rebuild)

For users who prefer editing directly instead of the app:

| Codec | Path |
|---|---|
| DTS | `/var/lib/webosbrew/dts25/gain.conf` |
| TrueHD/MLP | `/var/lib/webosbrew/truehd/gain.conf` |

**Full format** (both files, same contract): a bare ASCII float on its own
line is the legacy make-up gain in dB (e.g. `6.0`) — unchanged, so an
existing single-line config keeps working exactly as before. Optionally,
additional `key=value` lines add DRC + dialogue boost:

```sh
mkdir -p /var/lib/webosbrew/dts25 /var/lib/webosbrew/truehd
cat > /var/lib/webosbrew/dts25/gain.conf <<'EOF'
6.0
drc=line
drc_boost=100
drc_cut=100
center=2.0
EOF
cat > /var/lib/webosbrew/truehd/gain.conf <<'EOF'
4.0
drc=rf
drc_boost=100
drc_cut=100
center=0.0
EOF
```

| Key | Meaning | Range | Default |
|---|---|---|---|
| *(bare float)* | make-up gain, dB | `-20.0..+20.0` | `0.0` |
| `drc` | DRC profile | `off` \| `line` \| `rf` | `off` |
| `drc_boost` | % of the curve's boost applied (mirrors LG's `drc_boost_scl_factor`) | `0..100` | `100` |
| `drc_cut` | % of the curve's cut applied (mirrors LG's `drc_cut_scl_factor`) | `0..100` | `100` |
| `center` | dialogue (front-centre channel) boost, dB | `-10.0..+10.0` | `0.0` |

**Preset shortcuts** (what the app's DRC stepper actually writes — same
result if you hand-edit `drc`/`drc_boost`/`drc_cut` to match):

| Preset | `drc` | `drc_boost` | `drc_cut` |
|---|---|---|---|
| Off    | `off`  | — | — |
| Light  | `line` | `50`  | `50`  |
| Medium | `line` | `100` | `100` |
| Night  | `rf`   | `100` | `100` |

`#` comments and blank lines ignored, whitespace tolerated, unknown keys
ignored (a newer config still reads on an older build). Missing file, empty
file, or an unparseable value → **that key's own default** — never fails
decode. Out-of-range *finite* values clamp to the nearest bound instead
(e.g. `center=-99` → `-10.0`); `nan`/`NaN` counts as unparseable (falls back
to default), not as an out-of-range number to clamp — see the docs'
"Config format" subsection for why that distinction matters. Takes effect on
the **next playback** (read once at decoder init) — no reboot or registry
re-init needed.

**How DRC interacts with make-up gain:** they are independent stages applied
in the same per-sample pass — `sample × DRC-gain × centre-gain (centre
channel only) × make-up-gain → clamp`. Make-up gain is a static level shift;
DRC dynamically compresses based on the signal; the two compose rather than
fight, and all three default to unity/off, so an un-tuned install is
**bit-identical** to a build with none of this code at all.

## 3. When you change the DSP source itself: test → rebuild → verify → recommit → tag

This section only applies if you edit the **code** that implements make-up
gain, DRC, or centre boost — `webos25/restore/src/gstdtsdec.c` or
`webos25/restore/build-truehd.sh` — not when just changing a `gain.conf`
value. Both files are **binary-affecting** per
[`../../.claude/rules/releasing.md`](../../.claude/rules/releasing.md): the
committed `.so` in `restore/{out,truehd-out}` are the source of truth and
must be kept in sync, on a real TV, in the same change.

0. **Run the desktop test suite first** (host-buildable, no cross-toolchain
   needed) — this is the pre-rebuild gate, catching most math/config
   regressions before you spend a cross-compile:
   ```sh
   src/test/run-tests.sh
   ```
   Compiles and runs the DRC unit tests against the DTS core extracted
   straight out of `gstdtsdec.c`, checks structural invariants (no
   transcendental math in the per-sample loop, no silence gate
   reintroduced, the NaN-safe clamp is used), and — because chunk 02 ported
   the DTS core byte-for-byte into the TrueHD patch — **diffs the DTS and
   TrueHD DRC cores and fails if they have drifted apart**, then separately
   unit-tests the TrueHD-specific host binding (block-window accumulation,
   patch-hunk arithmetic). A green run here does not replace on-device
   verification below, but a red run means don't bother cross-building yet.
1. **Rebuild both binaries:**
   ```sh
   ./build.sh          # -> out/libgstdtsdec.so, out/libdca.so.0
   ./build-truehd.sh   # -> truehd-out/libgstlibav.so + libav*/libsw* (applies the mlpdec gain patch)
   ```
2. **Check each build's `BUILD-REPORT.txt`** (`out/BUILD-REPORT.txt`,
   `truehd-out/BUILD-REPORT.txt`) — confirms the ELF is armel soft-float
   (`e_flags 0x05000200`) and every GLIBC symbol is `<= 2.35`, matching the C5
   userspace (see [`../README.md#target-abi-the-other-crux`](../README.md#target-abi-the-other-crux)).
   `build-truehd.sh` also self-verifies the patch, generated to
   `/tmp/mlpdec-webos25-loudness.patch` and applied with `git apply`
   (fallback `patch -p1`): a pre-apply scope assertion that the patch touches
   only `libavcodec/mlpdec.c` (`build-truehd.sh:1268-1283`), then **8 checks**
   after applying it (`build-truehd.sh:1294-1321`) — 6 `verify_has` presence
   checks (make-up gain, the truehd `gain.conf` path, the ported DRC core,
   the DRC curve, the per-sample apply, the level detector), one asserting
   the retired silence gate has NOT been reintroduced, and one asserting the
   detector reads samples before the gain is applied (feed-forward) — before
   it produces output.
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
