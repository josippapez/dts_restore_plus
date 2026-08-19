/* =====================================================================
 * DTS Enabler (universal) - privileged backend JS service
 * ---------------------------------------------------------------------
 * One app, every rooted LG generation. This service:
 *   1. DETECTS the TV at runtime (arch / float-ABI / GStreamer / how LG
 *      disabled DTS) and derives a machine profile.
 *   2. BRANCHES on that profile to apply the correct DTS-restore
 *      mechanism.
 *
 * It holds NO elevation of its own. Every privileged action is executed
 * as root by shelling the command out to the Homebrew Channel (HBC) exec
 * service:
 *
 *     luna://org.webosbrew.hbchannel.service/exec  { "command": "<sh>" }
 *
 * which returns { stdout, stderr, returnValue }.
 *
 * SUPPORTED PROFILES (see MULTI-MODEL.md / EPIC.md):
 *
 *   webos25-armel-gst124  (LG C5 etc.)  -- VERIFIED mechanism (DTS + TrueHD).
 *       "decoder-inject": stage TWO payloads --
 *         DTS:    patched libgstdtsdec.so (+ libdca.so.0, S32LE output) to
 *                 /var/lib/webosbrew/dts25.
 *         TrueHD: our libgstlibav.so (avdec_truehd/avdec_mlp) + minimal ffmpeg
 *                 libs to /var/lib/webosbrew/truehd.
 *       Then apply THREE bind-mounted overrides + regenerate the registry:
 *         (1) bind our libgstlibav over LG's TrueHD-less one;
 *         (2) add TRUEHD+MLP to the umediaserver codec capability config
 *             (edit the TV's live /etc file with awk -- ship no LG config);
 *         (3) add avdec_truehd/mlp=310 to gstcool.conf [sw_decoder] (the SW-rank
 *             lever that makes LG autoplug the SW decoder, not its HW path);
 *         (4) regenerate the media GStreamer registry (dtsdec + avdec_truehd,
 *             LD_LIBRARY_PATH=truehd/libs) and write it to
 *             /mnt/flash/data/gst_1_0_registry.arm.bin.
 *       Overrides (1)-(3) are bind-mounts. (4) is NOT: the plugin registry is a
 *       persistent `cp -f` over the media registry, which no unmount can revert,
 *       so Disable/Uninstall revert it by regenerating a clean STOCK registry
 *       from LG's own plugins. A bind whose target is busy (a live mapping such
 *       as WebAppMgr holding the .so) is detached lazily rather than left
 *       applied. Both codecs proven on a real LG C5.
 *       Mirrors webos25/install.sh + webos25/init_dts25.sh exactly.
 *
 *   cx-armv7-gst114       (OLED CX class) -- UNVERIFIED (no CX hardware).
 *       "demuxer-override": bind-mount rebuilt LG demuxer/libav .so's over
 *       /usr/lib/gstreamer-1.0/, refresh the registry, and bump
 *       avdec_dca 0->290 (+ [downmix]) in gstcool.conf. Mirrors the
 *       repo-root install.sh / init_dts.sh.
 *
 *   unknown / unknown-*   -- REFUSE. We never apply an ABI-mismatched
 *       mechanism (applying CX 1.14 libs on a 1.24 TV would break MKV/MP4
 *       playback -- see MULTI-MODEL.md sec 3.1).
 *
 * SECURITY MODEL
 *   - Everything handed to exec runs as ROOT.
 *   - This app takes NO caller-controlled shell input: there are no
 *     free-form parameters on any method. Every path, filename, rank and
 *     downmix coefficient below is a hardcoded constant validated at
 *     author time. Nothing user-, network-, or client-supplied is ever
 *     interpolated into a command string, so the injection surface is
 *     empty by construction.
 *   - The detected profile is the ONLY value that steers behaviour, and
 *     it is matched against a fixed allowlist (PROFILE_* constants) before
 *     any mechanism runs; an unrecognised profile is refused, never
 *     interpolated.
 *   - Generated init scripts are written via `base64 -d` heredocs so no
 *     content survives the write as shell syntax.
 *   - ONE method takes a caller-settable value: `enable {force: true}`, the
 *     explicit opt-in for a TV that is not in the verified-fingerprint table.
 *     It is validated as `payload.force === true` (the literal boolean only --
 *     no truthy strings, no numbers) and is NEVER interpolated into a command
 *     string. It selects between two author-constant script texts, `FORCE=1`
 *     and `FORCE=0`, and is honoured only when the read-only probe already
 *     reported COMPAT_CANFORCE=1 with the loader gate passing. So the statement
 *     above still holds literally: no caller-supplied value reaches the shell.
 *
 * COMPATIBILITY GATE (webOS 25)
 *   Our .so are bind-mounted over LG's system-wide -- the overrides propagate
 *   into every app jail (27 jail-side binds per library, measured on a C5) --
 *   so "probably compatible" is not good enough. Enable and the boot hook both
 *   refuse unless:
 *     (1) the GStreamer major.minor still matches the build (ABI guard, never
 *         overridable);
 *     (2) the md5s of the three stock plugins we shadow are in init_dts25.sh's
 *         verified-sets table -- or the user opted in and they have not changed
 *         since (stock.fp). A change UNDER an existing install is "drift" and is
 *         refused unconditionally: FORCE cannot override it, because uninstall
 *         clears the baseline and puts the TV back in the unverified flow, which
 *         already has a consented opt-in;
 *     (3) every payload object's dynamic dependencies resolve on THIS TV. The
 *         check asserts OUR deps resolve, NOT that our sonames match LG's:
 *         stock libav on the verified C5 is ffmpeg 5.x while ours is 4.4, so
 *         soname equality would refuse a TV where the payload demonstrably
 *         works;
 *     (4) after binding, the regenerated registry still carries dtsdec,
 *         avdec_truehd, qtdemux, tsdemux and matroskademux -- otherwise the
 *         binds are dropped and the registry is not committed.
 *   Every refusal path binds nothing, exits 0, AND repairs the media registry if
 *   one of ours is still live (it is `cp -f`'d, not bind-mounted, so it survives
 *   any unmount and would keep our dtsdec registered by absolute path). The gate
 *   is authored once, in W25_COMPAT_SH, and spliced into the probe, Enable,
 *   Disable and the boot script alike.
 * ===================================================================== */

"use strict";

var Service = require("webos-service");

/* md5 of a string, used only to cross-check the installed boot script against the
 * one this build would write (see hookStaleness). Guarded because the gate-stamp
 * comparison is the primary signal and must not be taken down by an unavailable
 * core module: without this, staleness is judged on the stamp alone. */
var md5hex = null;
try {
  var _crypto = require("crypto");
  md5hex = function (str) { return _crypto.createHash("md5").update(str, "utf8").digest("hex"); };
} catch (e) {
  md5hex = null;
}

var PKG_ID = "io.github.josippapez.dtsenabler.service";
var service = new Service(PKG_ID);

var HBC_EXEC = "luna://org.webosbrew.hbchannel.service/exec";

/* ---- App install tree (payload ships under the APP, not the service) ---- */
// On webOS the app and its JS service install into SEPARATE trees, so the
// service cannot reach the payload via a path relative to __dirname. Address
// the application install dir explicitly.
var APP_ID       = "io.github.josippapez.dtsenabler";
// The app dir differs by install type: homebrew/dev apps live under
// /media/developer/apps/..., production apps under /usr/palm/applications/...
// Resolve it at runtime in the shell via $APPBASE (defined by APPBASE_PRELUDE,
// which every payload-using builder prepends). PAYLOAD_* therefore carry a shell
// variable reference, expanded inside the double-quoted command strings.
var APPBASE_PRELUDE =
  'APPBASE=/media/developer/apps/usr/palm/applications/' + APP_ID + '; ' +
  '[ -d "$APPBASE" ] || APPBASE=/usr/palm/applications/' + APP_ID;
var APP_INSTALL  = "$APPBASE";
var PAYLOAD_W25     = APP_INSTALL + "/payload/webos25";          // libgstdtsdec.so + libdca.so.0
var PAYLOAD_W25_THD = APP_INSTALL + "/payload/webos25-truehd";   // libgstlibav.so + ffmpeg libs
var PAYLOAD_W25_DMX = APP_INSTALL + "/payload/webos25-demux";    // patched isomp4 + mpegtsdemux
var PAYLOAD_CX      = APP_INSTALL + "/payload/cx";               // CX demuxer/libav .so set
var PAYLOAD_TESTS   = APP_INSTALL + "/payload/testfiles";        // DTS container samples (self-test)

var LOG = "/tmp/dtsenabler.log";

/* ---- Profile names (fixed allowlist) ---------------------------------- */
var PROFILE_W25 = "webos25-armel-gst124";
var PROFILE_CX  = "cx-armv7-gst114";

/* =======================================================================
 * webOS 25 unified DTS + TrueHD/MLP mechanism constants
 * (mirror webos25/install.sh + the canonical webos25/init_dts25.sh)
 * ===================================================================== */
var W25_DEST        = "/var/lib/webosbrew/dts25";
var W25_LIBS        = W25_DEST + "/libs";
var W25_INIT_SCRIPT = W25_DEST + "/init_dts25.sh";
/* State files the boot script owns. Spelled out here rather than reached for
 * through the shell variables W25_COMPAT_SH happens to define ($FP,
 * $CLI_MARKER): a builder that splices that block is doing so for its helper
 * FUNCTIONS, and depending on its variables would mean an unrelated rename over
 * there silently turns these two lines into no-ops. */
var W25_STOCK_FP    = W25_DEST + "/stock.fp";
var W25_CLI_MARKER  = W25_DEST + "/.cli-install";
var W25_HOOK        = "/var/lib/webosbrew/init.d/restore_dts25";
var W25_REG_TARGET  = "/mnt/flash/data/gst_1_0_registry.arm.bin";
var W25_REG_TMP     = "/tmp/gst_dts_reg.bin";
/* TrueHD side: our gst-libav + ffmpeg libs, plus the two /etc config overrides
 * we generate by editing the TV's own live files and bind-mount over them. */
var W25_THD_DEST    = "/var/lib/webosbrew/truehd";
var W25_THD_LIBS    = W25_THD_DEST + "/libs";
var W25_CFG_LIVE    = "/etc/umediaserver/device_codec_capability_config.json";
var W25_CFG_OVR     = W25_THD_DEST + "/codec_capability.json";
var W25_GC_LIVE     = "/etc/gst/gstcool.conf";
var W25_GC_OVR      = W25_THD_DEST + "/gstcool.conf";
var W25_LGLIBAV     = "/usr/lib/gstreamer-1.0/libgstlibav.so";
/* Container-demuxer side: patched isomp4/mpegtsdemux (dts_support default TRUE)
 * so DTS works in mp4/ts/m2ts, not just MKV. Staged then bind-mounted over LG's
 * demuxers BEFORE the registry regen (fully reversible). */
var W25_DMX_DEST    = "/var/lib/webosbrew/demux25";
var W25_ISO_LIVE    = "/usr/lib/gstreamer-1.0/libgstisomp4.so";
var W25_TSD_LIVE    = "/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so";
/* awk programs that generate the two overrides (same logic as install.sh;
 * written to the TV via base64 heredoc + run with `awk -f` to avoid any
 * shell quoting hazard). Author constants only -- nothing caller-supplied. */
var W25_CAP_AWK = [
  '/"name" : "DTSE"/ { indts=1 }',
  '{ print }',
  'indts && /^ *},/ {',
  '  print "    {";',
  '  print "      \\"name\\" : \\"TRUEHD\\",";',
  '  print "      \\"channels\\" : 8";',
  '  print "    },";',
  '  print "";',
  '  print "    {";',
  '  print "      \\"name\\" : \\"MLP\\",";',
  '  print "      \\"channels\\" : 8";',
  '  print "    },";',
  '  indts=0',
  '}'
].join("\n");
var W25_GC_AWK = [
  '{ print }',
  '/^\\[sw_decoder\\]/ { print "avdec_truehd=310"; print "avdec_mlp=310" }'
].join("\n");

/* =======================================================================
 * The canonical boot/apply script, split into three MACHINE-GENERATED line
 * arrays taken verbatim from webos25/restore/init_dts25.sh (CLAUDE.md rule 3:
 * this file, that file and install.sh's INIT_B64 blob must render byte-for-byte
 * identical text -- `restore/check-init-sync.sh` proves it).
 *
 * W25_COMPAT_SH is the middle slice: the compatibility gate and the
 * reversibility helpers. DETECT_PROBE, w25Enable() and w25DisableSteps() all
 * splice that same slice in, so the verdict the UI shows, the gate Enable
 * enforces and the gate the boot hook enforces are one authored copy of the
 * logic rather than three that drift.
 *
 * Do not hand-edit these arrays: edit webos25/restore/init_dts25.sh and
 * regenerate them.
 * ===================================================================== */
var W25_INIT_HEAD = [
  "#!/bin/sh",
  "# webOS25 DTS + TrueHD restore. Runs at boot via /var/lib/webosbrew/init.d/restore_dts25.",
  "#",
  "# THREE BYTE-IDENTICAL COPIES (CLAUDE.md rule 3): this file, the base64 INIT_B64",
  "# blob in restore/install.sh, and w25InitScriptBody() in app/service/service.js.",
  "# The block between the W25-COMPAT markers is additionally shared verbatim with",
  "# the app's read-only DETECT_PROBE, so the verdict the UI shows is the verdict",
  "# this script enforces. Change one copy -> change all three.",
  "#",
  "# MODES",
  "#   (default)     apply: self-heal check (unowned -> revert+unlink; owned but the",
  "#                 core payload is incomplete -> refuse, delete nothing) -> version",
  "#                 guard -> identity gate -> loader gate -> binds -> post-bind",
  "#                 proof -> registry commit.",
  "#   W25_CHECK=1   read-only preflight for install.sh / the app's Enable: prints",
  "#                 VERDICT/REASON/LABEL/CANFORCE/LOADER and exits. Binds nothing,",
  "#                 writes nothing, toasts nothing.",
  "#   W25_STAND_DOWN=1",
  "#                 put this TV back on stock and exit: drop our binds and rebuild",
  "#                 LG's registry if one of ours is live. No gate, no binds, no",
  "#                 toast, and it can never apply anything. This is what the two",
  "#                 installers call when their preflight refuses AFTER they have",
  "#                 already detached the binds, so \"nothing is applied\" is true",
  "#                 rather than merely intended.",
  "#   FORCE=1       explicit opt-in on a TV that is not in the verified table.",
  "#",
  "# Always exits 0: a non-zero webosbrew init script trips the failsafe that",
  "# disables ALL root customisations on the next boot. Every refusal path binds",
  "# nothing -- the overrides are system-wide (they propagate into every app jail),",
  "# so \"not sure\" must mean \"do not touch this TV\". The two container demuxers are",
  "# OPTIONAL: without them DTS still works in MKV, which is how this shipped before",
  "# the gate existed, so each is bound only when staged.",
  "#",
  "# A refusal also REPAIRS: it drops the binds and, if a media registry we wrote is",
  "# still live, regenerates LG's. That second half is not optional -- the registry",
  "# is committed with `cp -f` rather than bind-mounted, so an unrepaired one keeps",
  "# our dtsdec registered by absolute path even with every bind gone.",
  "#",
  "# STAGED FILES ARE LEFT IN PLACE ON A REFUSAL, DELIBERATELY. A refused install",
  "# links no boot hook, so nothing of ours ever runs again on that TV; the files",
  "# under /var/lib/webosbrew are inert without the binds. They are kept because",
  "# stock.fp is among them, and that recorded baseline is the only way a later boot",
  "# can tell \"the firmware changed\" from \"this TV was never verified\" -- deleting it",
  "# would downgrade every future drift into a bare \"unverified\". The app's Uninstall",
  "# removes them on demand.",
  "set -u",
  "LOG=/tmp/dts25.log",
  "W25_LOG=$LOG",
  "toast() { luna-send -n 1 luna://com.webos.notification/createToast \"{\\\"sourceId\\\":\\\"io.github.josippapez.dtsenabler\\\",\\\"message\\\":\\\"$1\\\"}\" >/dev/null 2>&1; }",
  "echo \"--- dts25+truehd $(date) ---\" >> $LOG 2>&1"
];

var W25_COMPAT_SH = [
  "# >>> W25-COMPAT-BEGIN",
  "# Compatibility gate + reversibility helpers -- the SINGLE AUTHORED COPY.",
  "# Everything here is either a pure measurement or an explicitly-called action;",
  "# merely sourcing this block mounts, deletes and writes nothing, which is what",
  "# lets the app's read-only probe reuse it as-is.",
  "EXPECT_GST=1.24",
  "# Stamp identifying the GATE this script enforces. The installed copy on a TV is",
  "# only rewritten by Enable / install.sh, so a TV enabled under an older app keeps",
  "# running that older script -- while the app's own probe judges with the compat",
  "# block embedded in the NEW build. The app compares this value (which it reads",
  "# from its own embedded copy of this block) against the one it finds in the",
  "# installed script, and reports the hook as stale instead of silently rewriting a",
  "# privileged file behind the user's back.",
  "#",
  "# BUMP THIS whenever the behaviour of this script changes. It is deliberately",
  "# independent of the app version: a cosmetic app release must not invalidate a",
  "# perfectly current hook, and a gate change must not hide behind an unchanged app",
  "# version. check-init-sync.sh proves the three IN-REPO copies match; this proves",
  "# the ON-TV copy matches the app that is asking, and sync-init.sh refuses to",
  "# regenerate the derived copies if the body moved while this did not.",
  "#   1  first gate: verified-sets table, loader gate, post-bind proof, self-heal,",
  "#      stand-down on every refusal, /etc fingerprints, hook stamp.",
  "#   2  w25_stock_registry refuses while a plugin bind of ours survives; the two",
  "#      installers stand the TV down on a refusal instead of leaving it half-way.",
  "W25_GATE_VERSION=2",
  "FP=/var/lib/webosbrew/dts25/stock.fp",
  "# Where the installed copy of THIS script lives, and the boot hook that symlinks",
  "# to it. Named here, in the shared block, so the read-only probe can fingerprint",
  "# the script and see whether the hook is linked without duplicating either path.",
  "INIT_SELF=/var/lib/webosbrew/dts25/init_dts25.sh",
  "HOOK=/var/lib/webosbrew/init.d/restore_dts25",
  "REG=/mnt/flash/data/gst_1_0_registry.arm.bin",
  "CFG=/etc/umediaserver/device_codec_capability_config.json",
  "GC=/etc/gst/gstcool.conf",
  "LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so",
  "LGISO=/usr/lib/gstreamer-1.0/libgstisomp4.so",
  "LGTSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so",
  "# CORE payload -- without either of these there is no DTS and no TrueHD, so a",
  "# missing one means \"do not bind anything\".",
  "MYDTS=/var/lib/webosbrew/dts25/libgstdtsdec.so",
  "MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so",
  "# OPTIONAL payload: the patched container demuxers. Absent, DTS still works in",
  "# MKV -- which is exactly how this shipped before the gate existed -- so each is",
  "# bound only when staged and is never a reason to refuse or to delete anything.",
  "MYISO=/var/lib/webosbrew/demux25/libgstisomp4.so",
  "MYTSD=/var/lib/webosbrew/demux25/libgstmpegtsdemux.so",
  "MYCFG=/var/lib/webosbrew/truehd/codec_capability.json",
  "MYGC=/var/lib/webosbrew/truehd/gstcool.conf",
  "MYLIBS=/var/lib/webosbrew/truehd/libs:/var/lib/webosbrew/dts25/libs",
  "w25_log() { [ -n \"${W25_LOG:-}\" ] || return 0; echo \"[dts25-gate $(date '+%Y-%m-%d %H:%M:%S')] $*\" >> \"${W25_LOG:-}\" 2>&1; }",
  "# Unmount ONE bind target, falling back to a LAZY detach when it is busy.",
  "# Measured on a real C5: umount of /usr/lib/gstreamer-1.0/libgstlibav.so fails",
  "# with \"target is busy\" because WebAppMgr has the .so mapped, while `umount -l`",
  "# succeeds. Without this fallback Disable only logs a WARN and silently leaves",
  "# the override applied.",
  "w25_umount() {",
  "  grep -q \" $1 \" /proc/mounts 2>/dev/null || { w25_log \"no bind over $1\"; return 0; }",
  "  if umount \"$1\" 2>/dev/null; then w25_log \"unmounted bind over $1\"; return 0; fi",
  "  if umount -l \"$1\" 2>/dev/null; then w25_log \"lazy-detached busy bind over $1 (a live mapping held it)\"; return 0; fi",
  "  w25_log \"WARN could not unmount $1, even lazily\"",
  "  return 1",
  "}",
  "# Drop every override WE applied -- rootfs paths only. The same binds propagate",
  "# into each app jail (27 jail-side copies per library on a real C5); those are",
  "# left alone deliberately, because detaching a jail's own view would break that",
  "# jail. $REG is included because older builds bind-mounted it.",
  "#",
  "# A target that survives even a lazy detach is REPORTED, not swallowed: it means",
  "# the revert did not actually happen, and silently claiming success is precisely",
  "# the class of failure this whole change exists to remove. Returns 1 and prints",
  "# WARN_UNMOUNT=<targets> so every caller -- boot script and app alike -- carries",
  "# the warning up.",
  "w25_drop_binds() {",
  "  UNMOUNT_FAILED=",
  "  for t in \"$CFG\" \"$GC\" \"$LGLIBAV\" \"$LGISO\" \"$LGTSD\" \"$REG\"; do",
  "    w25_umount \"$t\" || UNMOUNT_FAILED=\"${UNMOUNT_FAILED:+$UNMOUNT_FAILED }$t\"",
  "  done",
  "  [ -z \"$UNMOUNT_FAILED\" ] && return 0",
  "  w25_log \"WARN revert incomplete -- still mounted: $UNMOUNT_FAILED\"",
  "  echo \"WARN_UNMOUNT=$UNMOUNT_FAILED\"",
  "  return 1",
  "}",
  "# THE refusal action. Standing down is not \"skip the binds\" -- it is \"leave this",
  "# TV stock\". The binds can be undone, but the media registry cannot: it is",
  "# committed with `cp -f`, and any registry WE generated was scanned with",
  "# /var/lib/webosbrew/dts25 on the plugin path, so it registers OUR dtsdec by",
  "# absolute path and keeps doing so system-wide (every one of the 27 jail views)",
  "# long after the binds are gone. Dropping binds while leaving that registry live",
  "# is exactly the state the gate exists to prevent -- and it is the state an OTA",
  "# that changes the stock plugins while GStreamer stays 1.24 would leave behind.",
  "#",
  "# Idempotent and self-limiting: w25_stock_registry scans LG's plugin directories",
  "# ONLY, so the registry it writes does not name /var/lib/webosbrew and the next",
  "# w25_reg_is_ours is false -- no repeated 60s regen on every boot. It also works",
  "# when our libraries are already gone, for the same reason.",
  "w25_stand_down() {",
  "  w25_drop_binds",
  "  if w25_reg_is_ours; then",
  "    w25_log \"a registry of ours is still live; regenerating the stock registry so nothing of ours stays registered\"",
  "    w25_stock_registry",
  "  fi",
  "  return 0",
  "}",
  "w25_md5() { [ -f \"$1\" ] || return 0; md5sum \"$1\" 2>/dev/null | cut -d\" \" -f1; }",
  "w25_bound() { if grep -q \" $1 \" /proc/mounts 2>/dev/null; then echo 1; else echo 0; fi; }",
  "w25_fp_get() { [ -f \"$FP\" ] || return 0; sed -n \"s/^$1=//p\" \"$FP\" 2>/dev/null | head -n1; }",
  "w25_gst_mm() { /usr/bin/gst-inspect-1.0 --version 2>/dev/null | sed -n 's/^GStreamer \\([0-9]*\\.[0-9]*\\).*/\\1/p' | head -n1; }",
  "w25_product_id() { command -v nyx-cmd >/dev/null 2>&1 || { echo unknown; return 0; }; v=$(nyx-cmd DeviceInfo query product_id 2>/dev/null | head -n1); [ -n \"$v\" ] || v=unknown; echo \"$v\"; }",
  "w25_webos_release() { command -v nyx-cmd >/dev/null 2>&1 || { echo unknown; return 0; }; v=$(nyx-cmd OSInfo query webos_release 2>/dev/null | head -n1); [ -n \"$v\" ] || v=unknown; echo \"$v\"; }",
  "# Measure the STOCK fingerprints of everything we shadow, plus the live GStreamer",
  "# version. While we are ENABLED a bind of ours shadows the target, so its live",
  "# hash is OURS and the stock hash is unmeasurable -- fall back to what stock.fp",
  "# recorded when it still was pristine. S_* = measured live (empty when bound),",
  "# M_* = effective stock fingerprint, B_* = 1 when our bind is present.",
  "#",
  "# The two /etc files matter as much as the plugins. $MYCFG and $MYGC are",
  "# SNAPSHOTS, awk-derived at install time from the TV's own live",
  "# device_codec_capability_config.json and gstcool.conf, and we bind them over the",
  "# originals indefinitely. Those originals only ever change via an OTA -- exactly",
  "# the event this gate exists to catch -- so an update that rewrites either one",
  "# while leaving the three plugins alone must not read as \"verified\": the hook",
  "# would silently revert LG's own config change, system-wide, forever.",
  "#",
  "# Residual we are NOT engineering around: libgstmatroska.so is neither shadowed",
  "# nor fingerprinted, so an OTA that changes its A_DTS retag would silently lose",
  "# MKV DTS. That fails in the acceptable direction -- it costs our codec and harms",
  "# nothing else -- and the five-element proof still passes, because it checks that",
  "# matroskademux REGISTERS, not what caps it emits.",
  "w25_measure() {",
  "  B_LIBAV=$(w25_bound \"$LGLIBAV\")",
  "  B_ISOMP4=$(w25_bound \"$LGISO\")",
  "  B_MPEGTS=$(w25_bound \"$LGTSD\")",
  "  B_CFG=$(w25_bound \"$CFG\")",
  "  B_GC=$(w25_bound \"$GC\")",
  "  S_LIBAV=",
  "  S_ISOMP4=",
  "  S_MPEGTS=",
  "  S_CFG=",
  "  S_GC=",
  "  [ \"$B_LIBAV\" = 0 ] && S_LIBAV=$(w25_md5 \"$LGLIBAV\")",
  "  [ \"$B_ISOMP4\" = 0 ] && S_ISOMP4=$(w25_md5 \"$LGISO\")",
  "  [ \"$B_MPEGTS\" = 0 ] && S_MPEGTS=$(w25_md5 \"$LGTSD\")",
  "  [ \"$B_CFG\" = 0 ] && S_CFG=$(w25_md5 \"$CFG\")",
  "  [ \"$B_GC\" = 0 ] && S_GC=$(w25_md5 \"$GC\")",
  "  M_LIBAV=$S_LIBAV",
  "  M_ISOMP4=$S_ISOMP4",
  "  M_MPEGTS=$S_MPEGTS",
  "  M_CFG=$S_CFG",
  "  M_GC=$S_GC",
  "  [ -n \"$M_LIBAV\" ] || M_LIBAV=$(w25_fp_get libgstlibav)",
  "  [ -n \"$M_ISOMP4\" ] || M_ISOMP4=$(w25_fp_get libgstisomp4)",
  "  [ -n \"$M_MPEGTS\" ] || M_MPEGTS=$(w25_fp_get libgstmpegtsdemux)",
  "  [ -n \"$M_CFG\" ] || M_CFG=$(w25_fp_get device_codec_capability_config)",
  "  [ -n \"$M_GC\" ] || M_GC=$(w25_fp_get gstcool)",
  "  GST_MM_NOW=$(w25_gst_mm)",
  "  [ -n \"$GST_MM_NOW\" ] || GST_MM_NOW=unknown",
  "  return 0",
  "}",
  "# Recorded-vs-measured comparison that treats \"we have no recorded value\" as",
  "# absence of evidence rather than as drift. Needed for the two /etc keys, which a",
  "# stock.fp written by an earlier build simply does not contain -- comparing an",
  "# empty recording against a real hash would report drift on every upgrade.",
  "w25_fp_differs() { [ -n \"$1\" ] && [ -n \"$2\" ] && [ \"$1\" != \"$2\" ]; }",
  "# Guard layer 0: our bind-over libs are armel GStreamer-1.24 builds. Binding",
  "# them over a different-ABI LG lib after an OTA would break ALL mp4/ts/mkv",
  "# playback, and no opt-in may override that.",
  "w25_gst_ok() { [ \"$GST_MM_NOW\" = \"$EXPECT_GST\" ]; }",
  "# Gate layer 1 -- identity. Sets VERDICT REASON LABEL CANFORCE from the measured",
  "# stock fingerprints:",
  "#   verified   the md5s are in the verified-sets table below",
  "#   forced     no table match, but the user opted in and nothing changed since",
  "#   drift      stock.fp recorded DIFFERENT md5s -> firmware update, stand down",
  "#              UNCONDITIONALLY (FORCE cannot override it, and neither can the",
  "#              ABI-change drift w25_gate reports)",
  "#   unverified no match and no opt-in -> bind nothing",
  "# The md5 of the stock libs is the key, not the model name: identical hashes",
  "# mean these are literally the libraries the payload was verified against.",
  "w25_verdict() {",
  "  VERDICT=unverified",
  "  REASON=",
  "  LABEL=",
  "  CANFORCE=0",
  "  if [ -z \"$M_LIBAV\" ] || [ -z \"$M_ISOMP4\" ] || [ -z \"$M_MPEGTS\" ]; then",
  "    REASON=\"the stock GStreamer plugin fingerprints could not be read on this TV\"",
  "    return 0",
  "  fi",
  "  # DRIFT IS CHECKED BEFORE THE TABLE, and that ordering is load-bearing. The",
  "  # table can only key on gst_mm + the three plugin md5s, so it cannot express the",
  "  # state of the two /etc files -- which means a table match would return",
  "  # `verified` and mask an OTA that rewrote only gstcool.conf or the codec",
  "  # capability JSON. \"Has this TV changed since we recorded it\" therefore outranks",
  "  # \"does this TV look like a known-good one\".",
  "  FP_AV=$(w25_fp_get libgstlibav)",
  "  FP_ISO=$(w25_fp_get libgstisomp4)",
  "  FP_TSD=$(w25_fp_get libgstmpegtsdemux)",
  "  FP_CFG=$(w25_fp_get device_codec_capability_config)",
  "  FP_GC=$(w25_fp_get gstcool)",
  "  # The three plugin hashes compare strictly (they have always been recorded).",
  "  # The two /etc hashes go through w25_fp_differs so an older stock.fp that never",
  "  # recorded them does not read as drift.",
  "  DRIFT_WHAT=",
  "  DRIFT_PLUGINS=0",
  "  DRIFT_CONFIG=0",
  "  [ \"$FP_AV\" != \"$M_LIBAV\" ] && { DRIFT_WHAT=\"${DRIFT_WHAT:+$DRIFT_WHAT }libgstlibav.so\"; DRIFT_PLUGINS=1; }",
  "  [ \"$FP_ISO\" != \"$M_ISOMP4\" ] && { DRIFT_WHAT=\"${DRIFT_WHAT:+$DRIFT_WHAT }libgstisomp4.so\"; DRIFT_PLUGINS=1; }",
  "  [ \"$FP_TSD\" != \"$M_MPEGTS\" ] && { DRIFT_WHAT=\"${DRIFT_WHAT:+$DRIFT_WHAT }libgstmpegtsdemux.so\"; DRIFT_PLUGINS=1; }",
  "  w25_fp_differs \"$FP_CFG\" \"$M_CFG\" && { DRIFT_WHAT=\"${DRIFT_WHAT:+$DRIFT_WHAT }device_codec_capability_config.json\"; DRIFT_CONFIG=1; }",
  "  w25_fp_differs \"$FP_GC\" \"$M_GC\" && { DRIFT_WHAT=\"${DRIFT_WHAT:+$DRIFT_WHAT }gstcool.conf\"; DRIFT_CONFIG=1; }",
  "  if [ -n \"$FP_AV$FP_ISO$FP_TSD\" ] && [ -n \"$DRIFT_WHAT\" ]; then",
  "    # UNCONDITIONAL refusal -- FORCE is deliberately not consulted here, and",
  "    # CANFORCE=0 so nothing advertises an escape hatch that does not exist.",
  "    # A drift-specific override would be redundant: uninstall removes stock.fp,",
  "    # so uninstall-then-enable puts this TV back into the \"unverified\" flow, which",
  "    # already has an explicit, consented opt-in. Refusing outright keeps the state",
  "    # machine smaller and the rule absolute: a firmware change that touches the",
  "    # plugins we shadow stands us down, full stop.",
  "    VERDICT=drift",
  "    CANFORCE=0",
  "    # The remedy depends on WHAT drifted. A plugin change is something a verified",
  "    # set can describe, so the fingerprints are worth reporting. A change to the",
  "    # two /etc files is NOT: no table row can ever clear it, because the table",
  "    # keys on plugins only -- the snapshots simply have to be retaken from the",
  "    # TV's new config, which is what Enable does.",
  "    if [ \"$DRIFT_PLUGINS\" = 1 ] && [ \"$DRIFT_CONFIG\" = 1 ]; then",
  "      DRIFT_FIX=\"uninstall then enable again to re-snapshot this TV's configuration, and report the new plugin fingerprints so this TV can be added to the verified table\"",
  "    elif [ \"$DRIFT_PLUGINS\" = 1 ]; then",
  "      DRIFT_FIX=\"report the new fingerprints so this TV can be added to the verified table, or uninstall then enable again to opt in explicitly\"",
  "    else",
  "      DRIFT_FIX=\"uninstall then enable again to re-snapshot this TV's configuration (no verified-set entry can cover a config change)\"",
  "    fi",
  "    REASON=\"this TV changed since it was last verified ($DRIFT_WHAT), so nothing was applied; $DRIFT_FIX\"",
  "    return 0",
  "  fi",
  "  while IFS='|' read -r t_mm t_av t_iso t_tsd t_label; do",
  "    case \"$t_mm\" in ''|\\#*) continue ;; esac",
  "    if [ \"$t_mm\" = \"$GST_MM_NOW\" ] && [ \"$t_av\" = \"$M_LIBAV\" ] && [ \"$t_iso\" = \"$M_ISOMP4\" ] && [ \"$t_tsd\" = \"$M_MPEGTS\" ]; then",
  "      VERDICT=verified",
  "      LABEL=$t_label",
  "      REASON=\"stock plugin fingerprints match a TV this payload was verified on\"",
  "      break",
  "    fi",
  "  done <<'W25_SETS'",
  "# gst_mm|libgstlibav|libgstisomp4|libgstmpegtsdemux|label",
  "1.24|0fd6d65ac9e3a78b393a615eaff8ac0b|57fe57060774f248c05af5a411fc9a8f|9b84a95cf29bc025553c7dee829b7cc1|LG C5 OLED77C51LA (webOS 10.3.1, GStreamer 1.24.0)",
  "W25_SETS",
  "  [ \"$VERDICT\" = verified ] && return 0",
  "  if [ \"$(w25_fp_get forced)\" = 1 ] && [ -n \"$FP_AV\" ]; then",
  "    VERDICT=forced",
  "    CANFORCE=1",
  "    REASON=\"not a verified TV, but you opted in and nothing has changed since\"",
  "    return 0",
  "  fi",
  "  if [ \"${FORCE:-0}\" = 1 ]; then",
  "    VERDICT=forced",
  "    CANFORCE=1",
  "    REASON=\"not a verified TV; applying because you explicitly opted in\"",
  "    return 0",
  "  fi",
  "  CANFORCE=1",
  "  REASON=\"this TV's stock GStreamer plugins match no TV this payload was verified on\"",
  "  return 0",
  "}",
  "# Version guard + identity gate as one call, for the read-only callers (the",
  "# app's probe and W25_CHECK=1). The apply path below runs the two as separate",
  "# ordered steps so each gets its own log line and toast.",
  "w25_gate() {",
  "  w25_gst_ok && { w25_verdict; return 0; }",
  "  VERDICT=drift",
  "  CANFORCE=0",
  "  LABEL=",
  "  REASON=\"GStreamer $GST_MM_NOW is not the $EXPECT_GST this payload was built for; a firmware update changed the plugin ABI\"",
  "  return 0",
  "}",
  "w25_loader() {",
  "  for f in /lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3 /lib/ld-linux.so.2 /lib/ld-linux*.so.* /lib/ld-*.so.*; do",
  "    [ -x \"$f\" ] && { printf '%s\\n' \"$f\"; return 0; }",
  "  done",
  "  return 1",
  "}",
  "# Gate layer 2a -- loader resolution. Every payload object we are about to bind",
  "# or register must have ALL its dynamic dependencies resolvable on THIS TV.",
  "# The assertion is that OUR deps resolve, NOT that our sonames match LG's:",
  "# stock libav on the verified C5 is ffmpeg 5.x (libavcodec.so.59,",
  "# libavformat.so.59, libavutil.so.57, libavfilter.so.8) while ours is ffmpeg 4.4",
  "# (.58/.58/.56/.7), so a soname-equality check against stock would refuse a TV",
  "# where the payload demonstrably works. Our objects carry",
  "# RUNPATH=/var/lib/webosbrew/truehd/libs; $MYLIBS is kept for the dts25/libs case.",
  "# LOADER_STAGED distinguishes \"these libraries cannot load on this TV\" (a real",
  "# refusal, and forcing would not help) from \"the core payload is not staged\" (the",
  "# state before the first Enable, where the answer is simply not known). The app",
  "# uses that to decide whether offering \"Try anyway\" would be honest.",
  "#",
  "# Only the CORE objects are required. The optional demuxers are checked when they",
  "# are staged and skipped when they are not, so a core-only install is a first",
  "# class configuration rather than a failure.",
  "w25_core_staged() { [ -f \"$MYDTS\" ] && [ -f \"$MYLIBAV\" ]; }",
  "w25_loader_ok() {",
  "  LOADER_MISS=",
  "  LOADER_STAGED=1",
  "  LD_SO=$(w25_loader)",
  "  if [ -z \"${LD_SO:-}\" ]; then LOADER_MISS=\"no dynamic loader found on this TV\"; return 1; fi",
  "  if ! w25_core_staged; then",
  "    LOADER_STAGED=0",
  "    LOADER_MISS=\"the core payload is not staged ($MYDTS / $MYLIBAV)\"",
  "    return 1",
  "  fi",
  "  for so in \"$MYDTS\" \"$MYLIBAV\" \"$MYISO\" \"$MYTSD\"; do",
  "    [ -f \"$so\" ] || continue",
  "    n=$(LD_LIBRARY_PATH=\"$MYLIBS\" LD_TRACE_LOADED_OBJECTS=1 \"$LD_SO\" \"$so\" 2>&1 | grep -c \"not found\")",
  "    if [ \"$n\" != 0 ]; then LOADER_MISS=\"$so has $n unresolved dependencies on this TV\"; return 1; fi",
  "  done",
  "  return 0",
  "}",
  "# Is the live media registry one WE wrote? It is committed with `cp -f`, not",
  "# bind-mounted, so there is no mount to look for -- but a registry we generated",
  "# names our plugin directories, and a stock one never does. This is the signal",
  "# for \"a registry of ours is live\", which matters when our libraries have gone",
  "# missing underneath it.",
  "w25_reg_is_ours() { [ -f \"$REG\" ] && grep -q \"/var/lib/webosbrew\" \"$REG\" 2>/dev/null; }",
  "# Gate layer 2b -- post-bind pipeline proof. The regenerated registry must carry",
  "# ALL FIVE elements the DTS/TrueHD path needs: our two decoders AND the three",
  "# demuxers we shadow (all three are present in the media registry today).",
  "# A missing demuxer means our override produced a plugin the registry cannot",
  "# use, i.e. a broken mp4/ts/mkv pipeline -- so the caller refuses the commit and",
  "# drops the binds, which turns \"the override didn't match\" into a plain no-op.",
  "w25_reg_has_all() {",
  "  REG_MISS=",
  "  for e in dtsdec avdec_truehd qtdemux tsdemux matroskademux; do",
  "    GST_REGISTRY_1_0=\"$1\" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 \"$e\" >/dev/null 2>&1 || { REG_MISS=$e; return 1; }",
  "  done",
  "  return 0",
  "}",
  "# Regenerate a clean STOCK registry from the pristine on-disk plugins and write",
  "# it over the media registry. The registry is committed with `cp -f` (a",
  "# PERSISTENT overwrite), not a bind-mount, so no umount can revert it; left",
  "# alone it keeps referencing removed /var/lib/webosbrew plugins and breaks",
  "# media-pipeline app audio (root-caused on a real C5, 2026-07-23). Call only",
  "# AFTER the binds are dropped. Same routine as uninstall.sh step 2b.",
  "#",
  "# RETURNS 0 ONLY IF LG'S REGISTRY IS ACTUALLY BACK IN PLACE. This is a cold-cache",
  "# full plugin scan under `timeout`, and at boot it runs at the busiest moment on",
  "# the box, so it genuinely can fail -- callers that go on to DELETE our plugins",
  "# must branch on this, never assume it worked. The scan's own exit status is not",
  "# sufficient evidence either: a truncated registry would be worse than none, so",
  "# the file has to be non-empty before it is committed.",
  "# Are any of the plugin binds we manage still mounted? w25_stock_registry has to",
  "# refuse while one is. The scan would then load OUR libgstlibav.so from the stock",
  "# path (its RUNPATH resolves fine), produce a syntactically valid registry that",
  "# happens to name no /var/lib/webosbrew path at all -- which CLEARS",
  "# w25_reg_is_ours, the very signal callers use to decide a repair is needed -- and",
  "# report success. A caller would take that as proof the TV is back on stock and",
  "# delete the payload out from under the surviving bind. Refusing here makes this",
  "# function's postcondition (\"$REG is a stock registry\") true by construction, and",
  "# every caller already handles a return of 1 by deferring instead of deleting.",
  "w25_plugin_binds_present() {",
  "  grep -q \" $LGLIBAV \" /proc/mounts 2>/dev/null && return 0",
  "  grep -q \" $LGISO \" /proc/mounts 2>/dev/null && return 0",
  "  grep -q \" $LGTSD \" /proc/mounts 2>/dev/null && return 0",
  "  return 1",
  "}",
  "w25_stock_registry() {",
  "  if w25_plugin_binds_present; then",
  "    w25_log \"WARN refusing to rebuild the stock registry: a plugin bind of ours is still mounted, so the scan would not be reading stock plugins\"",
  "    return 1",
  "  fi",
  "  CLEAN_REG=/tmp/gst_clean_reg.bin",
  "  rm -f \"$CLEAN_REG\" 2>/dev/null",
  "  if GST_REGISTRY_1_0=\"$CLEAN_REG\" GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0 GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 60 /usr/bin/gst-inspect-1.0 >/dev/null 2>&1 && [ -s \"$CLEAN_REG\" ]; then",
  "    if cp -f \"$CLEAN_REG\" \"$REG\" 2>/dev/null; then",
  "      w25_log \"regenerated clean stock registry over $REG\"",
  "      rm -f \"$CLEAN_REG\" 2>/dev/null",
  "      return 0",
  "    fi",
  "    w25_log \"WARN could not write the clean stock registry to $REG\"",
  "  else",
  "    w25_log \"WARN clean stock registry regen failed (scan timed out, errored, or produced nothing); leaving $REG untouched\"",
  "  fi",
  "  rm -f \"$CLEAN_REG\" 2>/dev/null",
  "  return 1",
  "}",
  "# Record the PRISTINE stock fingerprints + TV identity. $1=verified, $2=forced.",
  "# Written only when the gate allows applying, and only from hashes taken while",
  "# no bind of ours shadowed the plugins -- that is what later lets a boot tell",
  "# \"the firmware changed\" apart from \"we are simply enabled\".",
  "w25_fp_write() {",
  "  mkdir -p /var/lib/webosbrew/dts25 2>/dev/null",
  "  { echo \"gst_mm=$GST_MM_NOW\"",
  "    echo \"product_id=$(w25_product_id)\"",
  "    echo \"webos_release=$(w25_webos_release)\"",
  "    echo \"libgstlibav=$M_LIBAV\"",
  "    echo \"libgstisomp4=$M_ISOMP4\"",
  "    echo \"libgstmpegtsdemux=$M_MPEGTS\"",
  "    echo \"device_codec_capability_config=$M_CFG\"",
  "    echo \"gstcool=$M_GC\"",
  "    echo \"verified=$1\"",
  "    echo \"forced=$2\"",
  "    echo \"written=$(date -u +%Y-%m-%dT%H:%M:%SZ)\"",
  "  } > \"$FP.tmp\" 2>/dev/null && mv -f \"$FP.tmp\" \"$FP\" 2>/dev/null \\",
  "    && w25_log \"wrote $FP (verified=$1 forced=$2)\" || w25_log \"WARN could not write $FP\"",
  "  return 0",
  "}",
  "# <<< W25-COMPAT-END"
];

var W25_INIT_MAIN = [
  "# W25_CHECK=1: read-only preflight used by install.sh and the app's Enable to",
  "# refuse with a readable reason BEFORE they link the boot hook. Prints only.",
  "if [ \"${W25_CHECK:-0}\" = 1 ]; then",
  "  w25_measure",
  "  w25_gate",
  "  echo \"GST_MM=$GST_MM_NOW\"",
  "  echo \"VERDICT=$VERDICT\"",
  "  echo \"REASON=$REASON\"",
  "  echo \"LABEL=$LABEL\"",
  "  echo \"CANFORCE=$CANFORCE\"",
  "  if w25_loader_ok; then echo \"LOADER=ok\"; else echo \"LOADER=$LOADER_MISS\"; fi",
  "  echo \"LOADER_STAGED=${LOADER_STAGED:-1}\"",
  "  # The six values a maintainer needs to add this TV to the verified-sets table.",
  "  # Empty md5s mean the plugin is currently shadowed by one of our binds, so the",
  "  # STOCK hash is unmeasurable right now -- run this with the overrides dropped.",
  "  echo \"PRODUCT_ID=$(w25_product_id)\"",
  "  echo \"WEBOS_RELEASE=$(w25_webos_release)\"",
  "  echo \"MD5_LIBGSTLIBAV=$S_LIBAV\"",
  "  echo \"MD5_LIBGSTISOMP4=$S_ISOMP4\"",
  "  echo \"MD5_LIBGSTMPEGTSDEMUX=$S_MPEGTS\"",
  "  echo \"MD5_DEVICE_CODEC_CAPABILITY_CONFIG=$S_CFG\"",
  "  echo \"MD5_GSTCOOL=$S_GC\"",
  "  # Which gate this script enforces, and its own fingerprint. \"$0\" is the",
  "  # installed path, so a caller can tell whether the script it just wrote is the",
  "  # one that ran.",
  "  SELF_MD5=$(w25_md5 \"$0\")",
  "  echo \"GATE_VERSION=$W25_GATE_VERSION\"",
  "  echo \"SCRIPT_MD5=$SELF_MD5\"",
  "  exit 0",
  "fi",
  "# W25_STAND_DOWN=1: revert to stock and exit. Deliberately a mode of its own",
  "# rather than \"just run the script and let it refuse\": a mode cannot accidentally",
  "# APPLY if the gate's inputs changed between the caller's preflight and this call.",
  "if [ \"${W25_STAND_DOWN:-0}\" = 1 ]; then",
  "  if w25_reg_is_ours; then WAS_OURS=1; else WAS_OURS=0; fi",
  "  w25_stand_down",
  "  w25_log \"stand-down requested by the installer (a registry of ours was live: $WAS_OURS)\"",
  "  echo \"STOOD_DOWN=$WAS_OURS\"",
  "  exit 0",
  "fi",
  "# --- G) self-heal / self-unlink, evaluated FIRST -----------------------------",
  "# TWO DIFFERENT FAULTS, TWO DIFFERENT ANSWERS. Deleting is only ever right when",
  "# nobody owns this install any more; a merely incomplete install is recoverable,",
  "# and destroying it would be worse than the hazard the guard exists for.",
  "#",
  "#   UNOWNED  the app dir is gone from BOTH install trees AND there is no CLI",
  "#            marker -> nobody can manage these system-wide overrides any more.",
  "#            Full heal: drop the binds, put a clean stock registry back, remove",
  "#            our state, unlink ourselves.",
  "#   INCOMPLETE (handled in the next block, not here) the install is still owned",
  "#            but a CORE object is missing -> bind nothing, delete nothing, keep",
  "#            the hook, and repair the registry if a stale one of ours is live.",
  "#",
  "# The hook stays a symlink to THIS file under /var/lib/webosbrew/dts25 rather",
  "# than into the app dir: a symlink into a removed app dir is dangling and",
  "# executes nothing, so it could never heal anything.",
  "APPDIR_DEV=/media/developer/apps/usr/palm/applications/io.github.josippapez.dtsenabler",
  "APPDIR_SYS=/usr/palm/applications/io.github.josippapez.dtsenabler",
  "CLI_MARKER=/var/lib/webosbrew/dts25/.cli-install",
  "w25_unowned() {",
  "  HEAL_WHY=",
  "  if [ ! -d \"$APPDIR_DEV\" ] && [ ! -d \"$APPDIR_SYS\" ] && [ ! -f \"$CLI_MARKER\" ]; then",
  "    HEAL_WHY=\"the DTS Enabler app is gone and this was not a CLI install\"",
  "    return 0",
  "  fi",
  "  return 1",
  "}",
  "# ORDER IS THE WHOLE POINT HERE: regenerate LG's registry FIRST and delete our",
  "# plugins only once that has actually succeeded.",
  "#",
  "# Deleting first would mean that a regen which times out -- a cold-cache full",
  "# plugin scan, at boot, on the busiest moment the box has -- leaves the live",
  "# registry naming plugins that no longer exist AND removes the hook that could",
  "# retry. That is the stale-registry state that broke other apps' audio on a real",
  "# C5 (2026-07-23), and LG's stack does not recover from it by itself.",
  "#",
  "# So on failure: keep the binds dropped, keep the state dirs, keep the hook, and",
  "# let the next boot try again. A heal that retries forever is strictly better",
  "# than one that breaks audio once. Unconditional regen (not w25_stand_down's",
  "# conditional one) because this path removes our plugins outright, so any",
  "# registry naming them must be replaced whether or not it looks like ours.",
  "w25_self_heal() {",
  "  w25_log \"SELF-HEAL: $HEAL_WHY -- reverting every override\"",
  "  w25_drop_binds",
  "  if ! w25_stock_registry; then",
  "    w25_log \"SELF-HEAL DEFERRED: LG's registry could not be rebuilt, so our plugins and the boot hook are KEPT (deleting them now would leave the live registry pointing at missing plugins). Retrying at the next boot.\"",
  "    toast \"DTS Enabler cleanup deferred: the TV was too busy to rebuild its plugin list. It will finish at the next restart.\"",
  "    return 1",
  "  fi",
  "  for d in /var/lib/webosbrew/dts25 /var/lib/webosbrew/truehd /var/lib/webosbrew/demux25; do",
  "    [ -d \"$d\" ] && rm -rf \"$d\"",
  "  done",
  "  rm -f \"$HOOK\"",
  "  toast \"DTS Enabler is gone: DTS/TrueHD overrides reverted and the boot hook removed.\"",
  "  return 0",
  "}",
  "# One compound command on purpose: the shell has finished parsing the whole",
  "# `if` (and both function bodies) before w25_self_heal deletes the directory",
  "# this very script is running from, so there is nothing left to read afterwards.",
  "if w25_unowned; then",
  "  if w25_self_heal; then echo \"HEALED=$HEAL_WHY\"; else echo \"HEAL_DEFERRED=$HEAL_WHY\"; fi",
  "  exit 0",
  "fi",
  "# --- G2) owned, but the CORE payload is incomplete ---------------------------",
  "# Recoverable, so nothing is deleted and the hook stays: re-opening the app or",
  "# re-running install.sh restages the payload and the next boot applies normally.",
  "# What DOES need repairing is the media registry -- it is committed with `cp -f`,",
  "# so a registry we wrote earlier survives independently of our files, and if it",
  "# is still live while our libraries are gone it names plugins that no longer",
  "# exist (that is the failure that broke media-pipeline app audio on a real C5,",
  "# 2026-07-23). Only the demuxers are optional; missing those is a normal",
  "# MKV-only install, not a fault.",
  "if ! w25_core_staged; then",
  "  w25_log \"REFUSED: core payload incomplete (dtsdec present=$([ -f \"$MYDTS\" ] && echo 1 || echo 0), libav present=$([ -f \"$MYLIBAV\" ] && echo 1 || echo 0)); binding nothing, install left intact\"",
  "  w25_stand_down",
  "  toast \"DTS Enabler: the installed files are incomplete, so nothing was applied. Re-open DTS Enabler (or re-run install.sh) to repair.\"",
  "  echo \"REFUSED=payload\"",
  "  echo \"REASON=the staged DTS/TrueHD core payload is incomplete; nothing was bound and nothing was removed\"",
  "  exit 0",
  "fi",
  "# --- measure the pristine stock fingerprints (read-only) --------------------",
  "w25_measure",
  "# --- GATE 0) firmware-update / ABI guard ------------------------------------",
  "if ! w25_gst_ok; then",
  "  w25_log \"ABORT: GStreamer '$GST_MM_NOW' != expected $EXPECT_GST (firmware update?); standing down\"",
  "  # The likeliest refusal in the wild, and the one where leaving a registry of",
  "  # ours live would be worst: our plugins may not even exist any more after a",
  "  # major firmware change. w25_stand_down handles that -- it rebuilds from LG's",
  "  # plugin directories only.",
  "  w25_stand_down",
  "  toast \"DTS/TrueHD paused: TV firmware changed (GStreamer $GST_MM_NOW). Re-open DTS Enabler to update.\"",
  "  echo \"REFUSED=drift\"",
  "  echo \"REASON=GStreamer $GST_MM_NOW is not the $EXPECT_GST this payload was built for; a firmware update changed the plugin ABI\"",
  "  exit 0",
  "fi",
  "# --- GATE 1) identity -------------------------------------------------------",
  "w25_verdict",
  "case \"$VERDICT\" in",
  "  verified|forced) w25_log \"gate: $VERDICT -- $REASON${LABEL:+ [$LABEL]}\" ;;",
  "  drift)",
  "    w25_log \"REFUSED: drift -- $REASON\"",
  "    w25_stand_down",
  "    toast \"DTS/TrueHD stopped: this TV changed since it was last verified, so nothing was applied. Uninstall then Enable in DTS Enabler to opt in again.\"",
  "    echo \"REFUSED=drift\"",
  "    echo \"REASON=$REASON\"",
  "    exit 0 ;;",
  "  *)",
  "    w25_log \"REFUSED: $VERDICT -- $REASON\"",
  "    w25_stand_down",
  "    toast \"DTS Enabler: this TV is not on the verified list, so nothing was changed. Open DTS Enabler to try anyway.\"",
  "    echo \"REFUSED=$VERDICT\"",
  "    echo \"REASON=$REASON\"",
  "    exit 0 ;;",
  "esac",
  "# --- GATE 2a) loader resolution ---------------------------------------------",
  "if ! w25_loader_ok; then",
  "  w25_log \"REFUSED: loader -- $LOADER_MISS\"",
  "  w25_stand_down",
  "  toast \"DTS Enabler: the payload cannot load on this TV ($LOADER_MISS); nothing was changed.\"",
  "  echo \"REFUSED=loader\"",
  "  echo \"REASON=$LOADER_MISS\"",
  "  exit 0",
  "fi",
  "# --- record the pristine fingerprints before anything of ours is bound ------",
  "case \"$VERDICT\" in",
  "  verified) FP_FORCED=$(w25_fp_get forced); [ -n \"$FP_FORCED\" ] || FP_FORCED=0; w25_fp_write 1 \"$FP_FORCED\" ;;",
  "  *)        w25_fp_write 0 1 ;;",
  "esac",
  "# --- APPLY 1) codec-capability override (adds TRUEHD/MLP so umediaserver allocates a decoder resource)",
  "[ -f \"$MYCFG\" ] && ! grep -q \" $CFG \" /proc/mounts 2>/dev/null && mount -n --bind \"$MYCFG\" \"$CFG\" 2>>$LOG",
  "# --- APPLY 2) replace LG.s truehd-less libav with ours (has avdec_truehd/avdec_mlp)",
  "[ -f \"$MYLIBAV\" ] && ! grep -q \" $LGLIBAV \" /proc/mounts 2>/dev/null && mount -n --bind -o ro \"$MYLIBAV\" \"$LGLIBAV\" 2>>$LOG",
  "# --- APPLY 2b) gstcool.conf: give avdec_truehd a high SW rank so LG autoplugs it (not the HW path)",
  "[ -f \"$MYGC\" ] && ! grep -q \" $GC \" /proc/mounts 2>/dev/null && mount -n --bind \"$MYGC\" \"$GC\" 2>>$LOG",
  "# --- APPLY 2c) container demuxers with DTS re-enabled (mp4/ts/m2ts DTS -> audio/x-dts).",
  "#         Patched isomp4/mpegtsdemux default dts_support=TRUE. Bound BEFORE the",
  "#         regen below so the registry picks them up at their normal path.",
  "[ -f \"$MYISO\" ] && ! grep -q \" $LGISO \" /proc/mounts 2>/dev/null && mount -n --bind -o ro \"$MYISO\" \"$LGISO\" 2>>$LOG",
  "[ -f \"$MYTSD\" ] && ! grep -q \" $LGTSD \" /proc/mounts 2>/dev/null && mount -n --bind -o ro \"$MYTSD\" \"$LGTSD\" 2>>$LOG",
  "# --- APPLY 3) regenerate the media registry (fresh) with dtsdec + our libav, then write it to the media path.",
  "#    Bounded by `timeout` and scanned in-process (GST_REGISTRY_FORK=no) so a hang can't trip HBC",
  "#    failsafe and no gst-plugin-scanner child lingers past the timeout.",
  "REG_TMP=/tmp/gst_dts_reg.bin",
  "rm -f \"$REG_TMP\"",
  "LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs \\",
  "GST_REGISTRY_1_0=\"$REG_TMP\" \\",
  "GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \\",
  "GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 30 /usr/bin/gst-inspect-1.0 >/dev/null 2>>$LOG",
  "# --- GATE 2b) post-bind pipeline proof, then commit: overwrite the media",
  "#         registry only if the regen survived the binds with all five elements",
  "#         intact. Otherwise drop our own binds -- that is what turns \"the",
  "#         override did not match\" from a broken media pipeline into a no-op.",
  "if w25_reg_has_all \"$REG_TMP\"; then",
  "  cp -f \"$REG_TMP\" \"$REG\" 2>>$LOG && echo \"registry updated (dtsdec+avdec_truehd+qtdemux+tsdemux+matroskademux)\" >>$LOG",
  "  echo \"APPLIED=$VERDICT\"",
  "else",
  "  w25_log \"REFUSED: regen is missing $REG_MISS; not committing it, and standing down\"",
  "  w25_stand_down",
  "  toast \"DTS Enabler: the plugin registry did not survive the override ($REG_MISS missing), so nothing was changed.\"",
  "  echo \"REFUSED=pipeline\"",
  "  echo \"REASON=the regenerated plugin registry is missing $REG_MISS\"",
  "fi",
  "exit 0"
];


/* =======================================================================
 * Make-up gain config files (per the EPIC config-file contract): a single
 * ASCII dB float, read by each decoder at init (dts25/gain.conf ->
 * gstdtsdec, truehd/gain.conf -> the ffmpeg mlpdec.c patch). Written from
 * the app via rootExec, mirroring the existing config-write pattern above
 * (mkdir -p the parent, write, no live-file editing needed here since
 * these are OUR files, not a bind-mount override of an LG /etc file).
 * No gstreamer registry re-init needed -- the decoder reads its config at
 * the NEXT playback init, so the value just needs to be on disk.
 * ===================================================================== */
var DTS_GAIN_CONF = W25_DEST + "/gain.conf";
var THD_GAIN_CONF = W25_THD_DEST + "/gain.conf";

/**
 * Clamp a caller-supplied gain to the contract range [-20, +20] dB and
 * round to 0.1 dB. Returns null (never NaN) for anything non-finite so
 * callers can reject bad input instead of silently writing garbage.
 */
function clampGainDb(v) {
  var n = Number(v);
  if (!isFinite(n)) return null;
  if (n < -20) n = -20;
  else if (n > 20) n = 20;
  return Math.round(n * 10) / 10;
}

/**
 * Clamp a caller-supplied center (dialogue) boost to the contract range
 * [-10, +10] dB and round to 0.1 dB. Returns null for non-finite input,
 * mirroring clampGainDb.
 */
function clampCenterDb(v) {
  var n = Number(v);
  if (!isFinite(n)) return null;
  if (n < -10) n = -10;
  else if (n > 10) n = 10;
  return Math.round(n * 10) / 10;
}

/* DRC preset -> config mapping (EPIC "Preset mapping", binding). Boost/cut
 * are irrelevant when mode is "off" (the decoder's inert guarantee bypasses
 * the whole DRC path), but we still write the contract's documented
 * defaults (100/100) for those fields rather than leaving them unset. */
var PRESET_MAP = {
  off:    { mode: "off",  boost: 100, cut: 100 },
  light:  { mode: "line", boost: 50,  cut: 50 },
  medium: { mode: "line", boost: 100, cut: 100 },
  night:  { mode: "rf",   boost: 100, cut: 100 }
};
var PRESET_ORDER = ["off", "light", "medium", "night"];

/** Validate a caller-supplied preset name against the enum. Returns the
 *  canonical key or null (never a caller-controlled string) so it can only
 *  ever reach the shell as one of the fixed PRESET_ORDER literals. */
function normalizePreset(v) {
  return (typeof v === "string" && PRESET_MAP.hasOwnProperty(v)) ? v : null;
}

/** Reverse-map a (mode, boost, cut) tuple read back from disk to the
 *  closest known preset name, for the UI to display. "off" always maps to
 *  Off regardless of boost/cut (they're inert there, per the decoders'
 *  inert guarantee). An exact match wins; otherwise fall back to the
 *  nearest named preset for that mode (Medium for a hand-edited "line",
 *  Night for a hand-edited "rf") so a config authored outside the app
 *  still shows a sane preset instead of nothing. */
function presetFromConfig(mode, boost, cut) {
  if (mode === "off") return "off";
  if (mode === "line") return (boost === 50 && cut === 50) ? "light" : "medium";
  if (mode === "rf") return "night";
  return "off";
}

/** Any DRC mode string other than "off"/"line"/"rf" (missing key, typo,
 *  hand-edit) falls back to "off", per the contract's "invalid -> default". */
function validDrcMode(v) { return (v === "off" || v === "line" || v === "rf") ? v : "off"; }

/** drc_boost / drc_cut percentage read back from disk; unparseable -> the
 *  contract default of 100. */
function drcPct(v) { return isFinite(parseFloat(v)) ? Math.round(Number(v)) : 100; }

/** Turn the KEY=VALUE output of w25GetGainScript() into the per-codec
 *  settings the UI (and the A/B preview) work in: {gain, preset, center}.
 *  Missing/unparseable values become the contract defaults (0 dB, preset
 *  Off, 0 dB centre), mirroring the decoders' own fault-tolerant parsing. */
function parseSavedConfig(kv) {
  function one(gainKey, drcKey, boostKey, cutKey, centerKey) {
    var gain = clampGainDb(parseFloat(kv[gainKey]));
    var center = clampCenterDb(parseFloat(kv[centerKey]));
    return {
      gain: gain === null ? 0 : gain,
      preset: presetFromConfig(validDrcMode(kv[drcKey]), drcPct(kv[boostKey]), drcPct(kv[cutKey])),
      center: center === null ? 0 : center
    };
  }
  return {
    dts: one("DTS_GAIN", "DTS_DRC", "DTS_BOOST", "DTS_CUT", "DTS_CENTER"),
    truehd: one("THD_GAIN", "THD_DRC", "THD_BOOST", "THD_CUT", "THD_CENTER")
  };
}

/** Write BOTH gain.conf files (temp file + mv so a decoder never reads a
 *  half-written file). mkdir -p the parent dirs first -- they may not
 *  exist yet if Enable was never run (decoders default to 0.0 dB unity
 *  regardless, per the contract's fault-tolerant default).
 *
 *  Each file gets the bare-float gain line FIRST (existing behavior,
 *  unchanged -- the decoders' first-data-line parse and w25ReadGain()
 *  below both depend on that), followed by the new `key=value` lines from
 *  the EPIC config contract. All substituted values are our own
 *  server-clamped numbers or one of the fixed PRESET_MAP mode strings, so
 *  nothing caller-controlled ever reaches the shell unescaped. */
function w25GainConfPrintf(path, gainDb, presetName, centerDb) {
  var p = PRESET_MAP[presetName];
  return 'printf "%s\\n%s\\n%s\\n%s\\n%s\\n" "' + gainDb.toFixed(1) + '" ' +
    '"drc=' + p.mode + '" "drc_boost=' + p.boost + '" "drc_cut=' + p.cut + '" ' +
    '"center=' + centerDb.toFixed(1) + '" > "' + path + '.tmp" && mv -f "' +
    path + '.tmp" "' + path + '"';
}

function w25GainConfWrite(path, gainDb, presetName, centerDb) {
  return w25GainConfPrintf(path, gainDb, presetName, centerDb) +
    ' || echo "FAIL: write ' + path + '"';
}

/* First-run audio defaults, seeded by Enable (and by restore/install.sh's
 * seed_gain_conf -- keep the two in sync, CLAUDE.md rule 3) when no
 * gain.conf exists yet.
 *
 * Gain and DRC shipped OPT-IN while the DSP was still unproven: no config
 * file meant fully inert (0.0 dB, DRC off), so enabling could not change
 * anyone's sound. Now that the curve is validated on-device, opt-in costs
 * more than it buys -- an untouched install leaves DTS/TrueHD quieter and
 * un-managed next to native AAC/AC-3, the very thing the feature fixes.
 * Users tune down from here rather than having to discover they must tune up.
 *
 * The values are EMPIRICAL, not theoretical: +5.0 dB with DRC line 100/100
 * is what the maintainer settled on by ear on a real C5 and ran as a hand-set
 * config (verified on-device 2026-07-29). It is also peak-safe by
 * construction -- line mode cuts 2:1 above -20 dBFS and limits 20:1 above
 * -10 dBFS -- and DRC-on mirrors LG's own native default for Dolby. */
var SEED_GAIN_DB = 5.0;
var SEED_PRESET = "medium";
var SEED_CENTER_DB = 0.0;

/** Seed one gain.conf, but ONLY when it does not exist yet -- Enable must
 *  never overwrite settings the user saved (re-running Enable is a normal
 *  recovery step). Logs rather than echoing, so Enable's stdout stays the
 *  bare "OK" its caller expects. */
function w25GainConfSeedScript(path) {
  return 'if [ -f "' + path + '" ]; then log "note: ' + path + ' exists; keeping saved audio settings"; ' +
    'elif ' + w25GainConfPrintf(path, SEED_GAIN_DB, SEED_PRESET, SEED_CENTER_DB) + '; then ' +
    'log "seeded first-run audio defaults -> ' + path + ' (+' + SEED_GAIN_DB.toFixed(1) +
    ' dB, DRC ' + PRESET_MAP[SEED_PRESET].mode + ' ' + PRESET_MAP[SEED_PRESET].boost +
    '/' + PRESET_MAP[SEED_PRESET].cut + ')"; ' +
    'else log "WARN: could not seed ' + path + '"; fi';
}

function w25SetGainScript(dtsDb, dtsPreset, dtsCenter, thdDb, thdPreset, thdCenter) {
  return [
    "set -u",
    'mkdir -p "' + W25_DEST + '" "' + W25_THD_DEST + '" || { echo "FAIL: mkdir"; exit 0; }',
    w25GainConfWrite(DTS_GAIN_CONF, dtsDb, dtsPreset, dtsCenter),
    w25GainConfWrite(THD_GAIN_CONF, thdDb, thdPreset, thdCenter),
    'echo OK',
    "exit 0"
  ].join("\n");
}

/** Read both gain.conf files back (for the UI to show the current value).
 *  Parses the same way the decoders do (see EPIC config-file contract): skip
 *  `#` comment lines and blank lines, take the first data line, strip its
 *  whitespace. So a hand-edited config with a comment reads back correctly. */
function w25ReadGain(path) {
  return "$(awk '/^[[:space:]]*#/{next} /^[[:space:]]*$/{next} " +
         "{gsub(/[[:space:]]/,\"\"); print; exit}' \"" + path + "\" 2>/dev/null)";
}

/** Read a single `key=value` line back out of a gain.conf, tolerating
 *  comments/blank lines and surrounding whitespace. Missing key -> empty
 *  string (the caller applies the contract's default). `key` is always one
 *  of our own literal constants, never caller input. */
function w25ReadKey(path, key) {
  // NB: copy $1 into a local (k) instead of gsub()-ing $1 directly. Assigning to
  // a field makes awk REBUILD $0 from the fields joined by OFS (a space), which
  // destroys the "=" -- so the later sub(/^[^=]*=/) matches nothing and the whole
  // "drc=rf" line comes back as "drcrf". That is not a BusyBox quirk; every awk
  // does it. Verified on the TV (BusyBox v1.35.0): buggy form -> "drcrf",
  // this form -> "rf".
  return "$(awk -F= '/^[[:space:]]*#/{next} { k=$1; gsub(/[[:space:]]/,\"\",k); " +
         'if (k == "' + key + '") { v=$0; sub(/^[^=]*=/,"",v); gsub(/[[:space:]]/,"",v); print v; exit } }\' "' +
         path + '" 2>/dev/null)';
}

function w25GetGainScript() {
  return [
    "DTS_GAIN=" + w25ReadGain(DTS_GAIN_CONF),
    "DTS_DRC=" + w25ReadKey(DTS_GAIN_CONF, "drc"),
    "DTS_BOOST=" + w25ReadKey(DTS_GAIN_CONF, "drc_boost"),
    "DTS_CUT=" + w25ReadKey(DTS_GAIN_CONF, "drc_cut"),
    "DTS_CENTER=" + w25ReadKey(DTS_GAIN_CONF, "center"),
    "THD_GAIN=" + w25ReadGain(THD_GAIN_CONF),
    "THD_DRC=" + w25ReadKey(THD_GAIN_CONF, "drc"),
    "THD_BOOST=" + w25ReadKey(THD_GAIN_CONF, "drc_boost"),
    "THD_CUT=" + w25ReadKey(THD_GAIN_CONF, "drc_cut"),
    "THD_CENTER=" + w25ReadKey(THD_GAIN_CONF, "center"),
    'echo "DTS_GAIN=$DTS_GAIN"',
    'echo "DTS_DRC=$DTS_DRC"',
    'echo "DTS_BOOST=$DTS_BOOST"',
    'echo "DTS_CUT=$DTS_CUT"',
    'echo "DTS_CENTER=$DTS_CENTER"',
    'echo "THD_GAIN=$THD_GAIN"',
    'echo "THD_DRC=$THD_DRC"',
    'echo "THD_BOOST=$THD_BOOST"',
    'echo "THD_CUT=$THD_CUT"',
    'echo "THD_CENTER=$THD_CENTER"',
    "exit 0"
  ].join("\n");
}

/* =======================================================================
 * CX mechanism constants  (mirror repo-root install.sh / init_dts.sh)
 * ===================================================================== */
var CX_STATE       = "/var/lib/webosbrew/dtsenabler/cx";
var CX_GST         = CX_STATE + "/gst";
var CX_INIT_SCRIPT = CX_STATE + "/init_dts.sh";
var CX_ENV_CONF    = CX_STATE + "/env.conf";
var CX_HOOK        = "/var/lib/webosbrew/init.d/restore_dts";
var CX_GST_TARGET  = "/usr/lib/gstreamer-1.0";
var GSTCOOL        = "/etc/gst/gstcool.conf";
var GSTCOOL_TMP    = "/tmp/gstcool.conf";
// The demuxer/libav .so set we bind-mount over LG's nerfed originals.
// libgstmpegtsdemux.so is optional (not shipped in every release); the loop
// silently skips any that are absent.
var CX_GST_LIBS = [
  "libgstisomp4.so",
  "libgstisomp4_1_8.so",
  "libgstmatroska.so",
  "libgstlibav.so",
  "libgstmpegtsdemux.so"
];
// Fixed stereo downmix coefficients (upstream dts_restore defaults). Author
// constants only -- never caller-supplied.
var CX_DOWNMIX = { front: "1.25", center: "0.75", lfe: "0.75", rear: "0.75", rear2: "0.70" };
var CX_DCA_RANK = "290";

/* =======================================================================
 * Root exec helper (hardened; carried over from the single-target app)
 * ===================================================================== */
/**
 * Run a shell command as root via the Homebrew Channel exec service.
 * Resolves on transport success with {stdout,stderr,returnValue}; rejects
 * only when the Luna call itself fails (HBC missing / TV not rooted).
 */
function rootExec(command) {
  return new Promise(function (resolve, reject) {
    service.call(HBC_EXEC, { command: command }, function (msg) {
      var p = (msg && msg.payload) ? msg.payload : {};
      // The Homebrew Channel exec service returns the output in `stdoutString` /
      // `stderrString` (plus base64 `stdoutBytes`/`stderrBytes`) -- NOT `stdout` /
      // `stderr`. Read the *String fields (fall back to the plain names in case a
      // future/other bridge uses them).
      var out = (p.stdoutString !== undefined) ? p.stdoutString : (p.stdout || "");
      var err = (p.stderrString !== undefined) ? p.stderrString : (p.stderr || "");
      var noOutput = p.stdout === undefined && p.stderr === undefined &&
                     p.stdoutString === undefined && p.stderrString === undefined;
      if (p.returnValue === false && noOutput) {
        reject({
          errorText: p.errorText || "exec call failed - is the Homebrew Channel installed and the TV rooted?",
          raw: p
        });
        return;
      }
      resolve({
        stdout: out,
        stderr: err,
        returnValue: p.returnValue !== false
      });
    });
  });
}

/* =======================================================================
 * Detection probe (embeds webos25/detect-target.sh logic, read-only)
 * ---------------------------------------------------------------------
 * Prints KEY=VALUE lines and a final PROFILE=. Reproduces the three axes
 * from MULTI-MODEL.md: (1) loader + ELF e_flags float ABI, (2) GStreamer
 * version, (3) how LG disabled DTS. It never mounts, copies or modifies
 * anything. This whole string is an author constant.
 * ===================================================================== */
var DETECT_PROBE = [
  'set -u',
  'first_glob() { for f in $1; do [ -e "$f" ] && { printf "%s\\n" "$f"; return 0; }; done; return 1; }',
  '',
  '# --- PROBE 1a: dynamic loader -> coarse arch + float ABI hint ---',
  'LOADER=unknown',
  'LD=$(first_glob "/lib/ld-linux*.so.* /lib/ld-linux-*.so.* /lib/ld-*.so.*" 2>/dev/null)',
  '[ -n "${LD:-}" ] && LOADER=$(basename "$LD")',
  'echo "LOADER=$LOADER"',
  '',
  '# --- PROBE 1b: ELF e_flags of a real gstreamer .so -> definitive float ABI ---',
  'EFLAGS=unknown; FLOAT_ABI=unknown',
  'GSTSO=$(first_glob "/usr/lib/gstreamer-1.0/libgstcoreelements.so /usr/lib/gstreamer-1.0/libgsttypefindfunctions.so /usr/lib/gstreamer-1.0/*.so" 2>/dev/null)',
  'if [ -n "${GSTSO:-}" ] && command -v od >/dev/null 2>&1; then',
  '  bytes=$(od -An -t x1 -j 36 -N 4 "$GSTSO" 2>/dev/null | tr -d " \\n")',
  '  if [ -n "$bytes" ] && [ "${#bytes}" -eq 8 ]; then',
  '    b0=$(printf "%s" "$bytes" | cut -c1-2); b1=$(printf "%s" "$bytes" | cut -c3-4)',
  '    b2=$(printf "%s" "$bytes" | cut -c5-6); b3=$(printf "%s" "$bytes" | cut -c7-8)',
  '    EFLAGS="0x${b3}${b2}${b1}${b0}"',
  '    val=$(printf "%d" "$EFLAGS" 2>/dev/null || echo 0)',
  '    if [ "$((val & 0x400))" -ne 0 ]; then FLOAT_ABI=hard',
  '    elif [ "$((val & 0x200))" -ne 0 ]; then FLOAT_ABI=soft',
  '    else FLOAT_ABI=unspecified; fi',
  '  fi',
  'fi',
  'echo "ELF_EFLAGS=$EFLAGS"',
  'echo "FLOAT_ABI=$FLOAT_ABI"',
  'echo "UNAME_M=$(uname -m 2>/dev/null || echo unknown)"',
  '',
  '# --- PROBE 2: GStreamer version -> plugin ABI + build system ---',
  'GST_VERSION=unknown',
  'if command -v gst-inspect-1.0 >/dev/null 2>&1; then',
  '  GST_VERSION=$(gst-inspect-1.0 --version 2>/dev/null | grep -i GStreamer | head -n1 | awk "{print \\$2}")',
  '  [ -n "$GST_VERSION" ] || GST_VERSION=unknown',
  'fi',
  'echo "GST_VERSION=$GST_VERSION"',
  'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
  'echo "GST_MAJMIN=${GST_MM:-unknown}"',
  '',
  '# --- PROBE 3: webOS release + product_id ---',
  'WEBOS_RELEASE=unknown; PRODUCT_ID=unknown',
  'if command -v nyx-cmd >/dev/null 2>&1; then',
  '  WEBOS_RELEASE=$(nyx-cmd OSInfo query webos_release 2>/dev/null | head -n1)',
  '  PRODUCT_ID=$(nyx-cmd DeviceInfo query product_id 2>/dev/null | head -n1)',
  '  [ -n "$WEBOS_RELEASE" ] || WEBOS_RELEASE=unknown',
  '  [ -n "$PRODUCT_ID" ] || PRODUCT_ID=unknown',
  'fi',
  'echo "WEBOS_RELEASE=$WEBOS_RELEASE"',
  'echo "PRODUCT_ID=$PRODUCT_ID"',
  '',
  '# --- PROBE 4: which DTS decoders, if any, are registered ---',
  'HAS_AVDEC_DCA=no; HAS_DTSDEC=no; HAS_DTS_AUDIODEC=no',
  'if command -v gst-inspect-1.0 >/dev/null 2>&1; then',
  '  gst-inspect-1.0 avdec_dca    >/dev/null 2>&1 && HAS_AVDEC_DCA=yes',
  '  gst-inspect-1.0 dtsdec       >/dev/null 2>&1 && HAS_DTSDEC=yes',
  '  gst-inspect-1.0 dts_audiodec >/dev/null 2>&1 && HAS_DTS_AUDIODEC=yes',
  'fi',
  'echo "HAS_AVDEC_DCA=$HAS_AVDEC_DCA"',
  'echo "HAS_DTSDEC=$HAS_DTSDEC"',
  'echo "HAS_DTS_AUDIODEC=$HAS_DTS_AUDIODEC"',
  '',
  '# --- PROBE 5: static matroskademux DTS-caps heuristic ---',
  'MKV_SO=$(first_glob "/usr/lib/gstreamer-1.0/libgstmatroska.so" 2>/dev/null)',
  'MKV_HAS_ADTS=unknown; MKV_HAS_XDTS=unknown',
  'if [ -n "${MKV_SO:-}" ] && command -v strings >/dev/null 2>&1; then',
  '  if strings "$MKV_SO" 2>/dev/null | grep -q "A_DTS"; then MKV_HAS_ADTS=yes; else MKV_HAS_ADTS=no; fi',
  '  if strings "$MKV_SO" 2>/dev/null | grep -q "audio/x-dts"; then MKV_HAS_XDTS=yes; else MKV_HAS_XDTS=no; fi',
  'fi',
  'echo "MKV_HAS_A_DTS_STRING=$MKV_HAS_ADTS"',
  'echo "MKV_HAS_XDTS_CAPS_STRING=$MKV_HAS_XDTS"',
  'MECH=unknown',
  'if [ "$MKV_HAS_ADTS" = "no" ]; then MECH=cx-demuxer-nerf',
  'elif [ "$MKV_HAS_ADTS" = "yes" ] && [ "$HAS_AVDEC_DCA" = "no" ] && [ "$HAS_DTSDEC" = "no" ] && [ "$HAS_DTS_AUDIODEC" = "no" ]; then MECH=webos25-retag-no-decoder',
  'elif [ "$MKV_HAS_ADTS" = "yes" ]; then MECH=demuxer-emits-dts; fi',
  'echo "DTS_DISABLE_MECHANISM_GUESS=$MECH"',
  '',
  '# --- PROFILE SELECTION ---',
  'PROFILE=unknown',
  'case "$GST_MM" in',
  '  1.14) PROFILE=cx-armv7-gst114 ;;',
  '  1.24)',
  '    if [ "$LOADER" = "ld-linux.so.3" ] && [ "$FLOAT_ABI" = "soft" ]; then PROFILE=webos25-armel-gst124',
  '    else PROFILE="webos25-${LOADER}-${FLOAT_ABI}"; fi ;;',
  '  *)',
  '    arch_tag="$LOADER"; [ "$arch_tag" = "unknown" ] && arch_tag=$(uname -m 2>/dev/null || echo arch)',
  '    PROFILE="unknown-gst${GST_MM}-${arch_tag}" ;;',
  'esac',
  'echo "PROFILE=$PROFILE"'
].concat(W25_COMPAT_SH, [
  "",
  "# --- PROBE 6: webOS 25 compatibility gate (read-only) ---",
  "# The W25-COMPAT block above is the boot script's gate verbatim. Only its",
  "# measurement + verdict functions are called here, so this stays read-only:",
  "# nothing is mounted, written or toasted.",
  "# While an override of OURS is bound, the plugin's live hash is ours and the",
  "# STOCK hash is unmeasurable -- hence <NAME>_BOUND alongside a <NAME>_MD5 that",
  "# is empty in that case, and a verdict derived from stock.fp rather than from a",
  "# live measurement.",
  "w25_measure",
  "echo \"LIBAV_BOUND=$B_LIBAV\"",
  "echo \"LIBAV_MD5=$S_LIBAV\"",
  "echo \"ISOMP4_BOUND=$B_ISOMP4\"",
  "echo \"ISOMP4_MD5=$S_ISOMP4\"",
  "echo \"MPEGTS_BOUND=$B_MPEGTS\"",
  "echo \"MPEGTS_MD5=$S_MPEGTS\"",
  "if [ -f \"$FP\" ]; then echo \"FP_PRESENT=1\"; else echo \"FP_PRESENT=0\"; fi",
  "echo \"FP_GST_MM=$(w25_fp_get gst_mm)\"",
  "echo \"FP_PRODUCT_ID=$(w25_fp_get product_id)\"",
  "echo \"FP_WEBOS_RELEASE=$(w25_fp_get webos_release)\"",
  "echo \"FP_LIBAV=$(w25_fp_get libgstlibav)\"",
  "echo \"FP_ISOMP4=$(w25_fp_get libgstisomp4)\"",
  "echo \"FP_MPEGTS=$(w25_fp_get libgstmpegtsdemux)\"",
  "echo \"FP_VERIFIED=$(w25_fp_get verified)\"",
  "echo \"FP_FORCED=$(w25_fp_get forced)\"",
  "echo \"FP_WRITTEN=$(w25_fp_get written)\"",
  "w25_gate",
  "echo \"COMPAT_VERDICT=$VERDICT\"",
  "echo \"COMPAT_REASON=$REASON\"",
  "echo \"COMPAT_LABEL=$LABEL\"",
  "echo \"COMPAT_CANFORCE=$CANFORCE\"",
  "if w25_loader_ok; then echo \"COMPAT_LOADER=ok\"; else echo \"COMPAT_LOADER=$LOADER_MISS\"; fi",
  "echo \"COMPAT_LOADER_STAGED=${LOADER_STAGED:-1}\"",
  "",
  "# --- PROBE 7: does the INSTALLED boot script enforce THIS app build's gate? ---",
  "# The on-TV script is only ever rewritten by Enable / install.sh, so a TV enabled",
  "# under an older app keeps running that older gate while this probe judges with",
  "# the freshly-embedded one. Report the mismatch; never silently rewrite a",
  "# privileged file just because someone opened the app.",
  "# $W25_GATE_VERSION here comes from the compat block spliced in above, i.e. it IS",
  "# this app build's value -- no second constant to keep in sync.",
  "HOOK_VER=",
  "HOOK_MD5=",
  "HOOK_PRESENT=0",
  "if [ -f \"$INIT_SELF\" ]; then",
  "  HOOK_PRESENT=1",
  "  HOOK_VER=$(sed -n \"s/^W25_GATE_VERSION=//p\" \"$INIT_SELF\" | head -n1)",
  "  HOOK_MD5=$(w25_md5 \"$INIT_SELF\")",
  "fi",
  "echo \"APP_GATE_VERSION=$W25_GATE_VERSION\"",
  "echo \"HOOK_SCRIPT_PRESENT=$HOOK_PRESENT\"",
  "echo \"HOOK_GATE_VERSION=$HOOK_VER\"",
  "echo \"HOOK_SCRIPT_MD5=$HOOK_MD5\"",
  "if [ -e \"$HOOK\" ]; then echo \"HOOK_LINKED=1\"; else echo \"HOOK_LINKED=0\"; fi",
  "echo \"CFG_BOUND=$B_CFG\"",
  "echo \"CFG_MD5=$S_CFG\"",
  "echo \"GC_BOUND=$B_GC\"",
  "echo \"GC_MD5=$S_GC\"",
  "echo \"FP_CFG=$(w25_fp_get device_codec_capability_config)\"",
  "echo \"FP_GC=$(w25_fp_get gstcool)\"",
  "",
  "# --- PROBE 8: does the STAGED payload match the one THIS app build ships? ---",
  "# App updates replace the bundled payload/** but never re-run Enable, so a TV",
  "# keeps running the .so it was last enabled with. PROBE 7 catches that for the",
  "# boot SCRIPT (via its version stamp); this catches it for the BINARIES, which",
  "# carry no version stamp at all -- hence md5. Both sides are on this TV (the",
  "# bundle under the app dir, the staged copy under /var/lib/webosbrew), so the",
  "# comparison is a local md5 of two files: no hash has to be embedded in the app.",
  "#",
  "# Read-only by construction: it measures and reports. It deliberately does NOT",
  "# re-stage -- that is Enable's job, and silently re-staging because someone",
  "# opened the app would re-apply a mechanism a user may have chosen to Disable",
  "# (the same reason PROBE 7 only reports).",
  "PD_DRIFT=",
  "PD_MISSING=",
  "PD_PRESENT=0",
  "PD_HASHABLE=1",
  "[ -n \"$(command -v md5sum 2>/dev/null)\" ] || PD_HASHABLE=0",
  "w25_pd_cmp() {",
  "  # $1=bundled file, $2=staged file, $3=short label",
  "  [ -f \"$1\" ] || return 0            # this build ships no such file -> nothing to compare",
  "  if [ ! -f \"$2\" ]; then PD_MISSING=\"$PD_MISSING $3\"; return 0; fi",
  "  PD_PRESENT=1",
  "  [ \"$PD_HASHABLE\" = \"1\" ] || return 0",
  "  pd_a=$(w25_md5 \"$1\"); pd_b=$(w25_md5 \"$2\")",
  "  [ -n \"$pd_a\" ] && [ -n \"$pd_b\" ] || return 0",
  "  [ \"$pd_a\" = \"$pd_b\" ] || PD_DRIFT=\"$PD_DRIFT $3\"",
  "}",
  "w25_pd_cmp \"" + PAYLOAD_W25 + "/libgstdtsdec.so\" \"" + W25_DEST + "/libgstdtsdec.so\" libgstdtsdec.so",
  "w25_pd_cmp \"" + PAYLOAD_W25 + "/libdca.so.0\" \"" + W25_LIBS + "/libdca.so.0\" libdca.so.0",
  "w25_pd_cmp \"" + PAYLOAD_W25_THD + "/libgstlibav.so\" \"" + W25_THD_DEST + "/libgstlibav.so\" libgstlibav.so",
  "w25_pd_cmp \"" + PAYLOAD_W25_DMX + "/libgstisomp4.so\" \"" + W25_DMX_DEST + "/libgstisomp4.so\" libgstisomp4.so",
  "w25_pd_cmp \"" + PAYLOAD_W25_DMX + "/libgstmpegtsdemux.so\" \"" + W25_DMX_DEST + "/libgstmpegtsdemux.so\" libgstmpegtsdemux.so",
  "# The ffmpeg libs move as a set with libgstlibav.so, so one representative is",
  "# enough to detect a TrueHD payload swap without hashing a dozen files.",
  "w25_pd_cmp \"" + PAYLOAD_W25_THD + "/libavcodec.so.58\" \"" + W25_THD_LIBS + "/libavcodec.so.58\" libavcodec.so.58",
  "echo \"PAYLOAD_STAGED_PRESENT=$PD_PRESENT\"",
  "echo \"PAYLOAD_HASHABLE=$PD_HASHABLE\"",
  "echo \"PAYLOAD_DRIFT_FILES=$(echo $PD_DRIFT)\"",
  "echo \"PAYLOAD_MISSING_FILES=$(echo $PD_MISSING)\""
]).join("\n");

/**
 * Run the read-only detection probe and parse its KEY=VALUE output.
 * @returns {Promise<{profile:string, probes:Object}>}
 */
function detectProfile() {
  return rootExec(DETECT_PROBE).then(function (r) {
    var kv = {};
    (r.stdout || "").split("\n").forEach(function (line) {
      var i = line.indexOf("=");
      if (i > 0) kv[line.slice(0, i).trim()] = line.slice(i + 1).trim();
    });
    return { profile: kv.PROFILE || "unknown", probes: kv };
  });
}

/** True for a profile we have a real, matched mechanism for. */
function isKnownProfile(profile) {
  return profile === PROFILE_W25 || profile === PROFILE_CX;
}

/**
 * Copy a compatVerdict() result onto a response, or leave the response untouched
 * when the gate does not apply (null). Keeping the keys ABSENT rather than null
 * is the point -- see the note in compatVerdict().
 */
function addCompatFields(res, c) {
  if (!c) return res;
  res.verdict = c.verdict;
  res.verdictReason = c.verdictReason;
  res.verifiedLabel = c.verifiedLabel;
  res.canForce = c.canForce;
  res.loaderResolves = c.loaderResolves;
  res.loaderDetail = c.loaderDetail;
  res.measured = c.measured;
  return res;
}

/**
 * Is the boot script installed on this TV the one THIS app build would write?
 *
 * It is only rewritten by Enable / install.sh, so after an app update a TV that
 * is already enabled keeps enforcing the OLD gate -- including its old
 * verified-sets table -- while the app's embedded probe judges with the new one.
 * In that window the two genuinely disagree, so it is reported rather than
 * papered over. Refreshing it is a privileged write and stays behind the user's
 * explicit Enable; opening the app is not consent.
 *
 * The stamp is the primary signal. The md5 is a cross-check that catches the case
 * the stamp cannot: a script whose behaviour changed without the stamp being
 * bumped. Both degrade to "not stale" rather than erroring when unavailable (no
 * crypto module, or no md5sum on the TV).
 *
 * A script that is not installed at all is NOT stale -- there is no older gate
 * running to disagree with. Neither is one whose hook symlink is gone (Disable
 * leaves the file behind, and Enable rewrites it anyway). A script present with NO
 * stamp is stale: that is exactly what an older build wrote. A script NEWER than
 * this app is reported as `hookNewer`, never as stale, because the remedy there is
 * to update the app -- advising Enable would downgrade the gate.
 *
 * Returns null for profiles without this mechanism (CX), so the caller omits the
 * fields entirely, the same way it does for the compatibility verdict.
 */
function hookStaleness(profile, kv, expectedMd5) {
  if (profile !== PROFILE_W25) return null;
  var appVer = kv.APP_GATE_VERSION || "";
  var hookVer = kv.HOOK_GATE_VERSION || "";
  var out = {
    appGateVersion: appVer || "unknown",
    hookGateVersion: hookVer || null,
    hookScriptInstalled: kv.HOOK_SCRIPT_PRESENT === "1",
    hookLinked: kv.HOOK_LINKED === "1",
    hookStale: false,
    hookNewer: false,
    hookStaleReason: ""
  };
  if (!out.hookScriptInstalled) {
    out.hookStaleReason = "No boot script is installed on this TV, so nothing is enforcing an older gate.";
    return out;
  }
  // Disabled on purpose: the script FILE survives Disable (only the hook symlink
  // goes), so nothing of it runs at boot and its version cannot matter yet. Enable
  // rewrites it anyway, so nagging here would offer a remedy whose real effect is
  // to re-enable a TV the user deliberately turned off.
  if (!out.hookLinked) {
    out.hookStaleReason = "DTS is disabled on this TV, so the installed boot script does not run. Enable will refresh it.";
    return out;
  }
  if (!hookVer) {
    out.hookStale = true;
    out.hookStaleReason = "The installed boot script carries no gate stamp, so an older build of this app wrote it. Enable to refresh it.";
    return out;
  }
  // NUMERIC, not string, and direction matters. The CLI tarball and the app ship
  // on independent tracks (HBC lag is normal), so a TV installed from a newer
  // tarball legitimately runs a gate AHEAD of this app. Calling that "stale" and
  // advising Enable would rewrite the newer script with this build's older gate --
  // a silent downgrade of the very guard being reported on.
  var hookN = /^[0-9]+$/.test(hookVer) ? parseInt(hookVer, 10) : null;
  var appN = /^[0-9]+$/.test(appVer) ? parseInt(appVer, 10) : null;
  if (hookN !== null && appN !== null && hookN > appN) {
    out.hookNewer = true;
    out.hookStaleReason = "The installed boot script enforces gate version " + hookVer +
      ", which is NEWER than this app build's gate version " + appVer +
      ". Update the app; do not re-Enable, which would replace it with the older gate.";
    return out;
  }
  if (hookN !== null && appN !== null ? hookN < appN : hookVer !== appVer) {
    out.hookStale = true;
    out.hookStaleReason = "The installed boot script enforces gate version " + hookVer +
      ", but this app build ships gate version " + appVer + ". Enable to refresh it.";
    return out;
  }
  if (expectedMd5 && kv.HOOK_SCRIPT_MD5 && kv.HOOK_SCRIPT_MD5 !== expectedMd5) {
    out.hookStale = true;
    out.hookStaleReason = "The installed boot script reports gate version " + appVer +
      " but its contents differ from what this app build would write. Enable to refresh it.";
    return out;
  }
  out.hookStaleReason = "The installed boot script matches this app build.";
  return out;
}

/** Copy a hookStaleness() result onto a response; no-op when it does not apply. */
function addHookFields(res, h) {
  if (!h) return res;
  res.hookStale = h.hookStale;
  res.hookNewer = h.hookNewer;
  res.hookStaleReason = h.hookStaleReason;
  res.hookGateVersion = h.hookGateVersion;
  res.appGateVersion = h.appGateVersion;
  res.hookScriptInstalled = h.hookScriptInstalled;
  res.hookLinked = h.hookLinked;
  return res;
}

/**
 * Is the payload STAGED on this TV the one this app build ships? (webOS 25 only)
 *
 * The companion to hookStaleness(). An app update replaces the bundled
 * payload/** but never re-runs Enable, so the TV keeps decoding with the .so it
 * was last enabled with -- silently, because unlike the boot script the binaries
 * carry no version stamp to compare. PROBE 8 md5s the bundled and staged copies
 * (both are on the TV) and reports which differ; this maps that onto the
 * detect/status contract.
 *
 * Reports only. Re-staging is Enable's job: doing it here would re-apply a
 * mechanism the user may have deliberately Disabled, the same trap hookStaleness()
 * documents for the hook-version nag.
 *
 * Returns null for profiles without this mechanism (CX), so the caller omits the
 * fields entirely, exactly as it does for the compatibility verdict.
 */
function payloadStaleness(profile, kv) {
  if (profile !== PROFILE_W25) return null;
  var drift = (kv.PAYLOAD_DRIFT_FILES || "").split(/\s+/).filter(Boolean);
  var missing = (kv.PAYLOAD_MISSING_FILES || "").split(/\s+/).filter(Boolean);
  var out = {
    payloadStale: false,
    payloadStaleFiles: drift,
    payloadStaleReason: ""
  };
  // Nothing staged at all => this TV was never enabled (or was uninstalled).
  // That is not drift, and telling someone their payload is stale when they have
  // not installed one would be nonsense. Enable is already the obvious next step.
  if (kv.PAYLOAD_STAGED_PRESENT !== "1") {
    out.payloadStaleReason = "No payload is staged on this TV yet, so there is nothing to compare.";
    return out;
  }
  if (kv.PAYLOAD_HASHABLE === "0") {
    out.payloadStaleReason = "No md5sum on this TV, so the staged payload could not be compared.";
    return out;
  }
  // A file this build ships that is absent from the staged set is a partial stage
  // (e.g. enabled by an older build that shipped no demuxers), which Enable fixes
  // the same way drift does -- so report it, but name it accurately.
  if (drift.length && missing.length) {
    out.payloadStale = true;
    out.payloadStaleReason = "The staged payload differs from this app build (" + drift.join(", ") +
      ") and is missing " + missing.join(", ") + ". Press Enable to re-stage it.";
    return out;
  }
  if (drift.length) {
    out.payloadStale = true;
    out.payloadStaleReason = "This app build ships newer " + drift.join(", ") +
      " than the copy staged on this TV. Press Enable to re-stage it.";
    return out;
  }
  if (missing.length) {
    out.payloadStale = true;
    out.payloadStaleReason = "This app build ships " + missing.join(", ") +
      (missing.length > 1 ? ", which are not staged on this TV. Press Enable to stage them."
                          : ", which is not staged on this TV. Press Enable to stage it.");
    return out;
  }
  out.payloadStaleReason = "The staged payload matches this app build.";
  return out;
}

/** Copy a payloadStaleness() result onto a response; no-op when it does not apply. */
function addPayloadFields(res, p) {
  if (!p) return res;
  res.payloadStale = p.payloadStale;
  res.payloadStaleReason = p.payloadStaleReason;
  res.payloadStaleFiles = p.payloadStaleFiles;
  return res;
}

/**
 * Map the probe's COMPAT_ and FP_ output onto the detect/status contract.
 *
 * `supported` (elsewhere) still means only "a mechanism exists for this
 * profile", so app.js keeps working; `verdict` is the separate question of
 * whether we are willing to apply that mechanism to THIS TV:
 *
 *   verified   stock plugin fingerprints are in the verified-sets table
 *   forced     no table match, but the user opted in and nothing changed since
 *   drift      the stock plugins changed since the install (firmware update);
 *              refused unconditionally, canForce is 0
 *   unverified no table match and no opt-in -- the one forceable verdict
 *   refused    no mechanism matches this TV's profile at all
 *
 * Returns NULL for the CX profile, meaning "this gate does not apply here", and
 * the caller then omits the verdict fields entirely. That is deliberate: the
 * fingerprint table, the md5s and the loader check are all webOS-25 artefacts, so
 * any verdict word borrowed for CX would be a false statement about a profile
 * that has a working mechanism -- and "unverified" in particular is what the UI
 * renders as "unsupported TV" beside a report block full of webOS-25 md5s. With
 * the fields absent, app.js's own fallback resolves CX to "verified" and the CX
 * display is unchanged. The genuine "never proven on CX hardware" caveat is
 * carried where it always was, by `status.verified` -> "NO - unverified on
 * hardware".
 *
 * `canForce` deliberately folds in the loader gate: offering "Try anyway" when
 * the payload's own dependencies cannot resolve on this TV would just produce a
 * second refusal. The gate shell keeps the two layers separate; only the
 * user-facing affordance combines them.
 *
 * The exception is the state before the first Enable, where the core payload is
 * not staged and the loader question is not answerable (COMPAT_LOADER_STAGED=0).
 * Treating that as "cannot load" would hide the opt-in behind an Enable the
 * verdict has already refused -- so it does not veto canForce. Enable stages the
 * payload and only then runs the real loader gate, before binding anything, so a
 * TV that genuinely cannot load the payload is still refused, with the reason.
 */
function compatVerdict(profile, kv) {
  var measured = {
    libgstlibav: kv.LIBAV_MD5 || null,
    libgstisomp4: kv.ISOMP4_MD5 || null,
    libgstmpegtsdemux: kv.MPEGTS_MD5 || null
  };
  if (profile === PROFILE_W25) {
    var loaderOk = kv.COMPAT_LOADER === "ok";
    var notStagedYet = !loaderOk && kv.COMPAT_LOADER_STAGED === "0";
    return {
      verdict: kv.COMPAT_VERDICT || "unverified",
      verdictReason: kv.COMPAT_REASON || "the compatibility probe returned no verdict",
      verifiedLabel: kv.COMPAT_LABEL || "",
      canForce: kv.COMPAT_CANFORCE === "1" && (loaderOk || notStagedYet),
      loaderResolves: loaderOk ? true : (notStagedYet ? null : false),
      loaderDetail: loaderOk ? "" : (kv.COMPAT_LOADER || "unknown"),
      measured: measured
    };
  }
  if (profile === PROFILE_CX) return null;
  return {
    verdict: "refused",
    verdictReason: "no verified DTS-restore mechanism matches this TV's profile '" + profile + "'.",
    verifiedLabel: "",
    canForce: false,
    loaderResolves: null,
    loaderDetail: "",
    measured: measured
  };
}

/* =======================================================================
 * webOS 25 mechanism shell builders  (mirror webos25/install.sh)
 * ===================================================================== */

/**
 * The canonical boot/apply script, rendered from the three generated arrays
 * above. Trailing newline included on purpose: webos25/restore/init_dts25.sh
 * ends with one, and "byte-identical" has to mean byte-identical.
 *
 * What it does at boot: full-heal if nobody owns the install any more, refuse
 * (without deleting anything) if the core payload is incomplete, refuse unless
 * this TV's stock plugin fingerprints are in the verified-sets table or the user
 * opted in, refuse unless every staged payload object's dynamic dependencies
 * resolve here, bind the overrides, and commit the regenerated registry only if
 * dtsdec, avdec_truehd, qtdemux, tsdemux and matroskademux all survived the
 * binds. Every refusal binds nothing, repairs the registry if one of ours is
 * live, and exits 0.
 */
function w25InitScriptBody() {
  return W25_INIT_HEAD.concat(W25_COMPAT_SH, W25_INIT_MAIN).join("\n") + "\n";
}

/** md5 of exactly the bytes Enable would write to the TV, for the hook-staleness
 *  cross-check. Computed once; null when no crypto module is available. */
var _expectedInitMd5;
function w25ExpectedInitMd5() {
  if (_expectedInitMd5 === undefined) {
    _expectedInitMd5 = md5hex ? md5hex(w25InitScriptBody()) : null;
  }
  return _expectedInitMd5;
}

/** enable (webOS 25): stage BOTH payloads, generate BOTH /etc overrides by
 *  editing the TV's own live files, install the canonical init script, run its
 *  read-only compatibility preflight, and only then link the boot hook, apply
 *  and restart. Mirrors webos25/install.sh.
 *
 *  `force` is a JS boolean the caller can only set via `enable {force: true}`
 *  (checked with `=== true` in runMechanism). It never reaches the shell: it
 *  picks one of exactly two author-constant lines, FORCE=1 or FORCE=0. Nothing
 *  is bound and the boot hook is not linked until the gate passes, so a refusal
 *  never APPLIES anything -- the staged files under /var/lib/webosbrew are inert
 *  without the bind-mounts, and are kept on purpose so stock.fp stays available
 *  as the drift baseline (Uninstall removes them).
 *
 *  It is not a no-op on a TV that was already enabled, though: step 3 has already
 *  detached the binds and our `cp -f` registry would still be live. Both refusal
 *  branches therefore stand the TV down and report STOOD_DOWN, so the outcome is
 *  "reverted to stock", not "half-way". */
function w25Enable(force) {
  var b64init = Buffer.from(w25InitScriptBody(), "utf8").toString("base64");
  var b64cap  = Buffer.from(W25_CAP_AWK, "utf8").toString("base64");
  var b64gc   = Buffer.from(W25_GC_AWK, "utf8").toString("base64");
  return [
    "set -u",
    APPBASE_PRELUDE,
    'LOG=' + LOG,
    'W25_LOG=' + LOG,
    'log() { echo "[dts25-install $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    // Author constant either way -- see the note on `force` above.
    (force ? "FORCE=1" : "FORCE=0"),
    'log "=== enable (webos25 DTS+TrueHD) start ==="',
    'log "app base: $APPBASE"',
    'log "force: $FORCE"'
  ].concat(W25_COMPAT_SH, [
    // Refusing after step 3 already detached the binds leaves the `cp -f` registry
    // as the only thing still applied, so put the TV fully back on stock and say
    // which of the two happened.
    'w25_refuse_stand_down() {',
    '  if w25_reg_is_ours; then WAS_OURS=1; else WAS_OURS=0; fi',
    '  w25_stand_down',
    '  log "refusal stand-down (a registry of ours was live: $WAS_OURS)"',
    '  echo "STOOD_DOWN=$WAS_OURS"',
    '}',
    // 1. Stage the DTS payload.
    'mkdir -p "' + W25_LIBS + '" || { log "FATAL: cannot create ' + W25_LIBS + '"; exit 0; }',
    // Claim ownership: the app is managing this install from now on. A leftover
    // .cli-install from an earlier SSH install would otherwise disable self-heal
    // forever for a user who has since switched to the app -- removing the app
    // would leave the system-wide overrides in place with nothing to manage them,
    // which is the exact hazard the self-heal exists for. install.sh writes the
    // marker back if the user returns to the CLI.
    'if [ -f "' + W25_CLI_MARKER + '" ]; then rm -f "' + W25_CLI_MARKER + '" && log "took ownership from a previous CLI install (removed ' + W25_CLI_MARKER + ')"; fi',
    'if [ -f "' + PAYLOAD_W25 + '/libgstdtsdec.so" ]; then',
    '  cp -f "' + PAYLOAD_W25 + '/libgstdtsdec.so" "' + W25_DEST + '/libgstdtsdec.so" && log "installed libgstdtsdec.so" || log "WARN: copy libgstdtsdec.so failed"',
    'else log "WARN: ' + PAYLOAD_W25 + '/libgstdtsdec.so not found (populate payload before packaging)"; fi',
    'if [ -f "' + PAYLOAD_W25 + '/libdca.so.0" ]; then',
    '  cp -f "' + PAYLOAD_W25 + '/libdca.so.0" "' + W25_LIBS + '/libdca.so.0" && log "installed libdca.so.0" || log "WARN: copy libdca.so.0 failed"',
    'else log "WARN: ' + PAYLOAD_W25 + '/libdca.so.0 not found (populate payload before packaging)"; fi',
    // 2. Stage the TrueHD payload (preserve the .so version symlinks).
    'mkdir -p "' + W25_THD_LIBS + '" || { log "FATAL: cannot create ' + W25_THD_LIBS + '"; exit 0; }',
    'if [ -f "' + PAYLOAD_W25_THD + '/libgstlibav.so" ]; then',
    '  cp -f "' + PAYLOAD_W25_THD + '/libgstlibav.so" "' + W25_THD_DEST + '/libgstlibav.so" && log "installed libgstlibav.so" || log "WARN: copy libgstlibav.so failed"',
    'else log "WARN: ' + PAYLOAD_W25_THD + '/libgstlibav.so not found (populate payload before packaging)"; fi',
    'n=0; for f in "' + PAYLOAD_W25_THD + '"/libav*.so* "' + PAYLOAD_W25_THD + '"/libsw*.so*; do [ -e "$f" ] && cp -Pf "$f" "' + W25_THD_LIBS + '/" && n=$((n+1)); done',
    'log "staged $n ffmpeg lib entries -> ' + W25_THD_LIBS + '"',
    // 2c. Stage the container-demuxer payload (optional; skipped if absent).
    'mkdir -p "' + W25_DMX_DEST + '" || log "WARN: cannot create ' + W25_DMX_DEST + '"',
    'for so in libgstisomp4.so libgstmpegtsdemux.so; do',
    '  if [ -f "' + PAYLOAD_W25_DMX + '/$so" ]; then cp -f "' + PAYLOAD_W25_DMX + '/$so" "' + W25_DMX_DEST + '/$so" && log "installed $so"; else log "note: ' + PAYLOAD_W25_DMX + '/$so absent; container DTS skipped"; fi',
    'done',
    // 2d. Seed first-run audio defaults (only when no config exists yet).
    w25GainConfSeedScript(DTS_GAIN_CONF),
    w25GainConfSeedScript(THD_GAIN_CONF),
    // 3. Unmount any stale binds so overrides are generated from PRISTINE /etc
    //    -- and so the preflight below fingerprints the PRISTINE stock plugins.
    //    w25_drop_binds falls back to a lazy detach (plain umount of
    //    libgstlibav.so fails with "target is busy" while WebAppMgr has it
    //    mapped) and prints WARN_UNMOUNT= for anything that survives even that.
    "w25_drop_binds",
    // 3a. Generate the codec-capability override (insert TRUEHD+MLP after DTSE).
    'base64 -d > /tmp/dts25_cap.awk <<\'B64CAP\'',
    b64cap,
    "B64CAP",
    'if [ -f "' + W25_CFG_LIVE + '" ]; then',
    '  if grep -q \'"TRUEHD"\' "' + W25_CFG_LIVE + '"; then cp -f "' + W25_CFG_LIVE + '" "' + W25_CFG_OVR + '"; log "capability already has TRUEHD; copied as-is";',
    '  else awk -f /tmp/dts25_cap.awk "' + W25_CFG_LIVE + '" > "' + W25_CFG_OVR + '" && log "generated capability override (TRUEHD+MLP after DTSE)" || log "WARN: capability override failed"; fi',
    'else log "WARN: ' + W25_CFG_LIVE + ' not present"; fi',
    // 3b. Generate the gstcool.conf override (avdec_truehd/mlp=310 rank lever).
    'base64 -d > /tmp/dts25_gc.awk <<\'B64GC\'',
    b64gc,
    "B64GC",
    'if [ -f "' + W25_GC_LIVE + '" ]; then',
    '  if grep -q \'^avdec_truehd=\' "' + W25_GC_LIVE + '"; then cp -f "' + W25_GC_LIVE + '" "' + W25_GC_OVR + '"; log "gstcool already has avdec_truehd; copied as-is";',
    '  else awk -f /tmp/dts25_gc.awk "' + W25_GC_LIVE + '" > "' + W25_GC_OVR + '" && log "generated gstcool override (avdec_truehd/mlp=310)" || log "WARN: gstcool override failed"; fi',
    'else log "WARN: ' + W25_GC_LIVE + ' not present"; fi',
    'rm -f /tmp/dts25_cap.awk /tmp/dts25_gc.awk 2>/dev/null',
    // 4. Write the canonical boot init script (base64 heredoc) + hook.
    'base64 -d > "' + W25_INIT_SCRIPT + '" <<\'B64EOF\'',
    b64init,
    "B64EOF",
    'chmod 0755 "' + W25_INIT_SCRIPT + '" && log "wrote ' + W25_INIT_SCRIPT + '"',
    // 4b. Compatibility preflight, READ-ONLY. Ask the script we just installed to
    //     run its own gate (W25_CHECK=1): fingerprint the three stock plugins we
    //     shadow, match them against the verified-sets table, and confirm every
    //     staged payload object's dynamic dependencies resolve on THIS TV. Asking
    //     the installed script keeps one authored copy of the gate instead of a
    //     second one here that could drift from it.
    'CHECK=$(W25_CHECK=1 FORCE="$FORCE" sh "' + W25_INIT_SCRIPT + '" 2>/dev/null)',
    'GATE_VERDICT=$(printf "%s\\n" "$CHECK" | sed -n "s/^VERDICT=//p" | head -n1)',
    'GATE_REASON=$(printf "%s\\n" "$CHECK" | sed -n "s/^REASON=//p" | head -n1)',
    'GATE_LABEL=$(printf "%s\\n" "$CHECK" | sed -n "s/^LABEL=//p" | head -n1)',
    'GATE_CANFORCE=$(printf "%s\\n" "$CHECK" | sed -n "s/^CANFORCE=//p" | head -n1)',
    'GATE_LOADER=$(printf "%s\\n" "$CHECK" | sed -n "s/^LOADER=//p" | head -n1)',
    'GATE_STAGED=$(printf "%s\\n" "$CHECK" | sed -n "s/^LOADER_STAGED=//p" | head -n1)',
    'log "preflight: verdict=$GATE_VERDICT canforce=$GATE_CANFORCE loader=$GATE_LOADER staged=$GATE_STAGED reason=$GATE_REASON"',
    // Refuse BEFORE the boot hook exists, so a refused TV is left with nothing
    // that runs at boot -- not even a hook that would re-refuse and re-toast.
    // LOADER_STAGED=0 means a CORE object never made it out of the app payload --
    // a packaging fault, reported as REFUSED=payload so it reads differently from
    // "this TV cannot load our libraries". A demux-less payload is NOT this case:
    // the demuxers are optional and the gate skips them when absent.
    'if [ "$GATE_LOADER" != "ok" ]; then',
    '  if [ "$GATE_STAGED" = 0 ]; then',
    '    log "REFUSED: core payload incomplete -- $GATE_LOADER"',
    '    echo "REFUSED=payload"',
    '  else',
    '    log "REFUSED: loader gate -- $GATE_LOADER"',
    '    echo "REFUSED=loader"',
    '  fi',
    '  echo "REASON=$GATE_LOADER"',
    '  w25_refuse_stand_down',
    '  exit 0',
    'fi',
    'case "$GATE_VERDICT" in',
    '  verified|forced) log "gate: $GATE_VERDICT -- $GATE_REASON${GATE_LABEL:+ [$GATE_LABEL]}" ;;',
    '  *)',
    '    log "REFUSED: $GATE_VERDICT -- $GATE_REASON"',
    '    echo "REFUSED=$GATE_VERDICT"',
    '    echo "REASON=$GATE_REASON"',
    '    echo "CANFORCE=$GATE_CANFORCE"',
    '    w25_refuse_stand_down',
    '    exit 0 ;;',
    'esac',
    // 4c. Link the boot hook (only now that the gate has passed).
    'mkdir -p "$(dirname "' + W25_HOOK + '")"',
    'if [ -L "' + W25_HOOK + '" ] || [ -e "' + W25_HOOK + '" ]; then rm -f "' + W25_HOOK + '"; fi',
    'ln -s "' + W25_INIT_SCRIPT + '" "' + W25_HOOK + '" && log "linked boot hook ' + W25_HOOK + '"',
    // 5. Apply now + restart the media pipeline. The init script re-runs the same
    //    gate, records the pristine fingerprints in stock.fp, binds, and commits
    //    the registry only if all five elements survive -- so it can still refuse
    //    here, on the post-bind pipeline proof, after standing itself down.
    'APPLY=$(FORCE="$FORCE" sh "' + W25_INIT_SCRIPT + '" 2>/dev/null)',
    'log "apply: $(printf "%s" "$APPLY" | tr "\\n" " ")"',
    'case "$APPLY" in',
    '  *REFUSED=*)',
    '    printf "%s\\n" "$APPLY" | grep -e "^REFUSED=" -e "^REASON=" -e "^WARN_UNMOUNT="',
    '    log "=== enable (webos25 DTS+TrueHD) refused on apply ==="',
    '    exit 0 ;;',
    'esac',
    'if killall starfish-media-pipeline 2>>"$LOG"; then log "restarted starfish-media-pipeline"; else log "note: media pipeline not running"; fi',
    'log "=== enable (webos25 DTS+TrueHD) done ==="',
    'echo "VERDICT=$GATE_VERDICT"',
    'echo "LABEL=$GATE_LABEL"',
    'echo OK',
    "exit 0"
  ]).join("\n");
}

/** The shared disable body, WITHOUT the terminating `echo OK` / `exit 0`, so
 *  uninstall can extend it by appending steps. It used to be built by regexing
 *  those two lines back off the finished disable script, which would have
 *  silently become a no-op the day that tail changed -- and a no-op there means
 *  uninstall quietly stops removing state.
 *
 *  Drops the boot hook, then every bind (lazily when a live mapping holds one
 *  busy), then puts a clean STOCK registry back. Both routines come from
 *  W25_COMPAT_SH, i.e. the same authored text the boot script uses. */
function w25DisableSteps() {
  return [
    "set -u",
    'LOG=' + LOG,
    'W25_LOG=' + LOG,
    'log() { echo "[dts25-disable $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    'log "=== disable (webos25) start ==="'
  ].concat(W25_COMPAT_SH, [
    'if [ -L "' + W25_HOOK + '" ] || [ -e "' + W25_HOOK + '" ]; then rm -f "' + W25_HOOK + '" && log "removed boot hook"; else log "boot hook not present"; fi',
    // Prints WARN_UNMOUNT=<targets> and returns non-zero if a target survived even
    // a lazy detach, i.e. the revert did not actually happen. Surfaced to the
    // caller below rather than swallowed.
    'w25_drop_binds',
    // The registry is written with `cp -f` (persistent), NOT bind-mounted -- so a
    // umount can never revert it (that was the bug: disable/uninstall left a stale
    // registry referencing our removed /var/lib/webosbrew libs, which breaks
    // media-pipeline app audio like Spotify until a valid registry is regenerated;
    // root-caused on a real C5, 2026-07-23). The binds are gone by now, so this
    // regenerates a clean STOCK catalog from the pristine on-disk plugins.
    //
    // Its RESULT is captured, not discarded: uninstall must not delete the plugins
    // the live registry still names if this failed, and a Disable whose regen
    // failed must not report clean either -- our dtsdec would stay registered AND
    // loadable, so DTS would keep working after a "successful" Disable.
    'if w25_stock_registry; then REG_REVERTED=1; else REG_REVERTED=0; fi',
    'echo "REG_REVERTED=$REG_REVERTED"',
    'rm -f "' + W25_REG_TMP + '" 2>/dev/null',
    'if killall starfish-media-pipeline 2>>"$LOG"; then log "restarted media pipeline"; else log "note: media pipeline not running"; fi',
    'log "=== disable (webos25) done ==="'
  ]);
}

/** disable (webOS 25). Keeps the staged libs AND stock.fp: Disable is meant to be
 *  reversed by Enable, and dropping stock.fp would also discard a recorded
 *  `forced=1` opt-in and the baseline that lets a later boot tell a firmware
 *  change apart from an unverified TV. */
function w25Disable() {
  return w25DisableSteps().concat([
    'echo OK',
    "exit 0"
  ]).join("\n");
}

/** uninstall (webOS 25): disable + remove all three state dirs and the recorded
 *  stock fingerprints. stock.fp lives inside dts25 so the rm -rf already covers
 *  it; removing it explicitly keeps the intent visible and still holds if that
 *  directory removal fails. Addressed by its own JS constant, never through the
 *  shell's $FP. */
function w25Uninstall() {
  return w25DisableSteps().concat([
    // DELETION IS GATED ON THE REGEN, exactly as uninstall.sh gates step 3 on
    // REG_OK. Deleting the plugins while the live registry still names them is the
    // failure that broke other apps' audio on a real C5 (2026-07-23) and that LG's
    // stack does not recover from -- so on failure keep everything and report a
    // deferral instead of "OK".
    'if [ "$REG_REVERTED" = 1 ]; then',
    '  rm -f "' + W25_STOCK_FP + '" 2>/dev/null',
    '  rm -rf "' + W25_DEST + '" "' + W25_THD_DEST + '" "' + W25_DMX_DEST + '" && echo "[dts25-uninstall] removed ' + W25_DEST + ' + ' + W25_THD_DEST + ' + ' + W25_DMX_DEST + '" >> "' + LOG + '" 2>&1',
    'else',
    '  log "DEFERRED: kept ' + W25_DEST + ', ' + W25_THD_DEST + ' and ' + W25_DMX_DEST + ' because LG registry rebuild failed"',
    '  echo "UNINSTALL_DEFERRED=1"',
    'fi',
    'echo OK',
    "exit 0"
  ]).join("\n");
}

/* =======================================================================
 * Self-test (webOS 25): decode each bundled DTS sample through the REAL
 * media registry and report PASS/FAIL per container. This exercises the
 * exact demux+decode chain the media pipeline uses, so a PASS means the
 * patch is actually working (independent of speakers / the output stage).
 * A decode that produces real PCM yields a multi-100KB WAV; a broken chain
 * (demuxer doesn't emit audio/x-dts, or no decoder) yields a header-only or
 * absent file. Author constants only.
 * ===================================================================== */
var TEST_WAV_MIN = 100000;   // bytes; a real decode is far larger, a fail is ~44 (header) or 0
var TEST_CASES = [
  { key: "mp4",  file: "DTS-in-mp4.mp4",     demux: "qtdemux" },
  { key: "ts",   file: "DTS-HD-MA-5.1.ts",   demux: "tsdemux" },
  { key: "m2ts", file: "DTS-HD-MA-5.1.m2ts", demux: "tsdemux" }
];
function w25SelfTest() {
  var lines = [
    "set -u",
    APPBASE_PRELUDE,
    'LOG=' + LOG,
    'REG=' + W25_REG_TARGET,
    'OUT=/tmp/dtsenabler_selftest.wav',
    'export LD_LIBRARY_PATH=' + W25_LIBS + ':' + W25_THD_LIBS,
    '# GST_REGISTRY_FORK=no per CLAUDE.md rule 4: without it a plugin-scanner fork can hold the'
    + '' ,
    '# HBC exec pipe open past gst-launch itself. timeout 60 (was 25): the FIRST case pays the'
    ,
    '# whole cold-start cost (registry + dtsdec + libdca load); 25s was measured too tight under'
    ,
    '# post-boot load on the real C5 (2026-08-18) while the decode itself was fine.'
    ,
    'export GST_REGISTRY_1_0="$REG" GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no',
    'echo "DTSDEC=$(gst-inspect-1.0 dtsdec >/dev/null 2>&1 && echo 1 || echo 0)"'
  ];
  TEST_CASES.forEach(function (t) {
    var f = PAYLOAD_TESTS + "/" + t.file;
    lines.push('F="' + f + '"');
    lines.push('rm -f "$OUT"');
    lines.push('if [ -f "$F" ]; then');
    lines.push('  timeout 60 gst-launch-1.0 -q filesrc location="$F" ! ' + t.demux + ' name=d d. ! queue ! dtsdec ! audioconvert ! wavenc ! filesink location="$OUT" >/dev/null 2>&1');
    lines.push('  SZ=$(stat -c%s "$OUT" 2>/dev/null || echo 0)');
    lines.push('  if [ "$SZ" -ge ' + TEST_WAV_MIN + ' ]; then echo "' + t.key + '=PASS:$SZ"; else echo "' + t.key + '=FAIL:$SZ"; fi');
    lines.push('else echo "' + t.key + '=MISSING:0"; fi');
  });
  lines.push('rm -f "$OUT" 2>/dev/null');
  lines.push("exit 0");
  return lines.join("\n");
}

/* =======================================================================
 * In-app A/B preview (webOS 25; DTS path only)
 * ---------------------------------------------------------------------
 * Renders the bundled DTS sample TWICE through `dtsdec` -- once with the
 * whole DRC/gain path inert (A: drc-mode=off, gain 0, centre 0) and once
 * with the user's SAVED gain.conf settings (B) -- so the difference can be
 * heard on identical content, back to back, without leaving the app.
 *
 * WHY PROPERTIES, NOT THE CONFIG FILE: every variant is expressed as
 * dtsdec GObject properties on the gst-launch command line (drc-mode,
 * drc-boost, drc-cut, makeup-gain-db, center-boost-db). gain.conf is
 * therefore never written, moved or even re-read, so the A/B cannot leave
 * the user's settings altered. The script hashes BOTH gain.conf files
 * before and after and returns the hashes so that is provable, not
 * merely asserted.
 *
 * WHERE THE RENDERED WAVs GO -- the app's own install dir, next to the
 * bundled samples ($APPBASE/payload/testfiles). Reason: the shipped
 * play-by-ear buttons already load `payload/testfiles/DTS-in-mp4.mp4` as
 * a RELATIVE URL from index.html and that works on the device, so a file
 * dropped in that same directory is reachable by the same mechanism.
 * /tmp has no such evidence behind it and is very likely outside the
 * app's document root, so it is not used. The directory is probed for
 * writability first and the call fails cleanly (never silently) if it is
 * read-only.
 *
 * VERIFIED ON DEVICE (C5, webOS 25): a 16-bit stereo RIFF/WAV from that
 * directory DOES play through the platform pipeline --
 *   gst-launch-1.0 playbin3 uri=file://<app>/payload/testfiles/..._a.wav
 * reaches EOS cleanly. The renders stay 16-bit stereo PCM (most broadly
 * supported WAV flavour, ~6x smaller than the native 5.1/S32 output);
 * both variants go through the identical downmix, so the A-vs-B
 * difference is preserved.
 *
 * WHY THE FILENAMES CARRY A STAMP: the player must never serve the
 * PREVIOUS render, but a `?r=<n>` cache-buster on the URL is fatal here.
 * webOS hands the <audio> src to starfish-media-pipeline, whose filesrc
 * URI handler does NOT strip the query, so it opens the literal path
 * "..._a.wav?r=1" and fails with "Resource not found" (reproduced with
 * playbin3 on the TV; that is the "player refused the clip" report).
 * A fresh basename per render gives a genuinely distinct URL with no
 * query string at all.
 *
 * MEASUREMENT: a second pass per variant through GStreamer's `level`
 * element (there is no ffmpeg on the TV), parsed from `gst-launch-1.0 -m`
 * bus messages. This pass deliberately does NOT include the stereo
 * capsfilter, so the numbers describe the decoder's native output. If
 * `level` is not registered on this TV the numbers are reported as "na"
 * rather than invented.
 * ===================================================================== */
var AB_SAMPLE    = "DTS-in-mp4.mp4";              // bundled clip used for both variants
var AB_PREFIX    = "dtsenabler_ab_";              // every render matches AB_PREFIX + "*.wav"
var AB_REL_URL   = "payload/testfiles/";          // relative to index.html (see above)
var AB_MIN_BYTES = TEST_WAV_MIN;                  // same "real PCM" floor as the self-test
var AB_TIMEOUT_S = 40;                            // per gst-launch run; a real render is ~seconds
var AB_LEVEL_NS  = 100000000;                     // `level interval=` -> one report per 100 ms

/* awk program that turns `gst-launch-1.0 -m` level messages into
 * "<tag>_MEAN=" (arithmetic mean of every per-channel rms dB value) and
 * "<tag>_PEAK=" (max per-channel peak dB). Tokens that are not numeric
 * (notably "-inf" for digital silence) are skipped rather than coerced to
 * 0, which would drag the mean up. Written to the TV via a base64 heredoc
 * and run with `awk -f`, the same way the install awk programs are, so no
 * part of it survives the write as shell syntax. */
var AB_LEVEL_AWK = [
  '{',
  '  n = index($0, "rms=(GValueArray)<"); if (n == 0) next;',
  '  s = substr($0, n + 18); e = index(s, ">"); if (e == 0) next; r = substr(s, 1, e - 1);',
  '  n = index($0, "peak=(GValueArray)<"); if (n == 0) next;',
  '  s = substr($0, n + 19); e = index(s, ">"); if (e == 0) next; p = substr(s, 1, e - 1);',
  '  c = split(r, a, ",");',
  '  for (i = 1; i <= c; i++) {',
  '    v = a[i]; sub(/^[^0-9+-]*/, "", v);',
  '    if (v !~ /^[+-]?[0-9]/) continue;',
  '    x = v + 0; if (x > -900) { sum += x; cnt++ }',
  '  }',
  '  c = split(p, a, ",");',
  '  for (i = 1; i <= c; i++) {',
  '    v = a[i]; sub(/^[^0-9+-]*/, "", v);',
  '    if (v !~ /^[+-]?[0-9]/) continue;',
  '    x = v + 0; if (!hp || x > pk) { pk = x; hp = 1 }',
  '  }',
  '}',
  'END {',
  '  if (cnt > 0) printf "%s_MEAN=%.1f\\n", t, sum / cnt; else printf "%s_MEAN=na\\n", t;',
  '  if (hp) printf "%s_PEAK=%.1f\\n", t, pk; else printf "%s_PEAK=na\\n", t;',
  '}'
].join("\n");

/** Build the dtsdec property list for one A/B variant. Every value is one
 *  of our own server-clamped numbers or a fixed PRESET_MAP mode literal,
 *  so nothing caller-controlled reaches the shell. */
function w25AbProps(gainDb, presetName, centerDb) {
  var p = PRESET_MAP[presetName];
  return [
    "drc-mode=" + p.mode,
    "drc-boost=" + p.boost,
    "drc-cut=" + p.cut,
    "makeup-gain-db=" + gainDb.toFixed(1),
    "center-boost-db=" + centerDb.toFixed(1)
  ].join(" ");
}

/**
 * The A/B render + measure script. `saved` is {gain, preset, center} as
 * read back from the DTS gain.conf, i.e. variant B. `nameA`/`nameB` are the
 * stamped basenames for this render (see AB_PREFIX above).
 *
 * Exit codes are captured straight after each `gst-launch` (never after a
 * pipe, where `$?` would be the parser's status), and every file is size-
 * checked before it is reported as rendered.
 */
function w25AbScript(saved, nameA, nameB) {
  var propsA = w25AbProps(0, "off", 0);
  var propsB = w25AbProps(saved.gain, saved.preset, saved.center);
  var b64awk = Buffer.from(AB_LEVEL_AWK, "utf8").toString("base64");
  return [
    "set -u",
    APPBASE_PRELUDE,
    'DIR="' + PAYLOAD_TESTS + '"',
    'SRC="$DIR/' + AB_SAMPLE + '"',
    'A="$DIR/' + nameA + '"',
    'B="$DIR/' + nameB + '"',
    'AWKF=/tmp/dtsenabler_ab_level.awk',
    'MSG=/tmp/dtsenabler_ab_level.txt',
    // dtsdec is NOT on the default plugin path, and libdca lives with our payload.
    // GST_REGISTRY_FORK=no keeps the plugin scanner in-process so it cannot hold
    // the HBC exec pipe open (see CLAUDE.md).
    'export LD_LIBRARY_PATH=' + W25_LIBS + ':' + W25_THD_LIBS,
    'export GST_PLUGIN_PATH=' + W25_DEST,
    'export GST_REGISTRY_FORK=no',
    // --- config fingerprints BEFORE anything runs -------------------------
    'conf_hash() { if [ -f "$1" ]; then h=$(md5sum "$1" 2>/dev/null | cut -d" " -f1); [ -n "$h" ] || h=nohash; else h=absent; fi; printf "%s" "$h"; }',
    'DTS_CONF_BEFORE=$(conf_hash "' + DTS_GAIN_CONF + '")',
    'THD_CONF_BEFORE=$(conf_hash "' + THD_GAIN_CONF + '")',
    'echo "DTS_CONF_BEFORE=$DTS_CONF_BEFORE"',
    'echo "THD_CONF_BEFORE=$THD_CONF_BEFORE"',
    'bail() { echo "ERR=$1"; echo "DIR=$DIR"; echo "DTS_CONF_AFTER=$DTS_CONF_BEFORE"; echo "THD_CONF_AFTER=$THD_CONF_BEFORE"; exit 0; }',
    // --- preconditions ----------------------------------------------------
    '[ -f "$SRC" ] || bail sample-missing',
    // Purge by prefix, not just this render's two names: earlier stamped takes
    // would otherwise pile up in the app dir.
    'rm -f "$DIR"/' + AB_PREFIX + '*.wav "$MSG" "$AWKF" 2>/dev/null',
    'PROBE="$DIR/.dtsenabler_ab_probe"',
    'if touch "$PROBE" 2>/dev/null; then rm -f "$PROBE" 2>/dev/null; else bail render-dir-readonly; fi',
    'if gst-inspect-1.0 level >/dev/null 2>&1; then HAVE_LEVEL=1; else HAVE_LEVEL=0; fi',
    'echo "LEVEL=$HAVE_LEVEL"',
    'base64 -d > "$AWKF" <<\'B64ABAWK\'',
    b64awk,
    "B64ABAWK",
    // --- render one variant to a 16-bit stereo WAV ------------------------
    'render() {',
    '  tag=$1; out=$2; shift 2',
    '  rm -f "$out" 2>/dev/null',
    '  timeout ' + AB_TIMEOUT_S + ' gst-launch-1.0 -q filesrc location="$SRC" ! qtdemux ! dtsdec "$@" ! audioconvert ! "audio/x-raw,format=S16LE,channels=2" ! wavenc ! filesink location="$out" >/dev/null 2>&1',
    '  rc=$?',
    '  sz=$(stat -c%s "$out" 2>/dev/null || echo 0)',
    '  echo "${tag}_RC=$rc"',
    '  echo "${tag}_BYTES=$sz"',
    '  echo "${tag}_PROPS=$*"',
    '}',
    // --- measure one variant with the `level` element ---------------------
    'measure() {',
    '  tag=$1; shift',
    '  if [ "$HAVE_LEVEL" != "1" ]; then echo "${tag}_MEAN=na"; echo "${tag}_PEAK=na"; return; fi',
    '  rm -f "$MSG" 2>/dev/null',
    '  timeout ' + AB_TIMEOUT_S + ' gst-launch-1.0 -m filesrc location="$SRC" ! qtdemux ! dtsdec "$@" ! audioconvert ! level interval=' + AB_LEVEL_NS + ' ! fakesink > "$MSG" 2>/dev/null',
    '  rc=$?',
    '  if [ "$rc" -eq 0 ] && [ -s "$MSG" ]; then awk -v t="$tag" -f "$AWKF" "$MSG"; else echo "${tag}_MEAN=na"; echo "${tag}_PEAK=na"; fi',
    '  rm -f "$MSG" 2>/dev/null',
    '}',
    'render A "$A" ' + propsA,
    'render B "$B" ' + propsB,
    'measure A ' + propsA,
    'measure B ' + propsB,
    'rm -f "$AWKF" 2>/dev/null',
    // --- config fingerprints AFTER ---------------------------------------
    'DTS_CONF_AFTER=$(conf_hash "' + DTS_GAIN_CONF + '")',
    'THD_CONF_AFTER=$(conf_hash "' + THD_GAIN_CONF + '")',
    'echo "DTS_CONF_AFTER=$DTS_CONF_AFTER"',
    'echo "THD_CONF_AFTER=$THD_CONF_AFTER"',
    "exit 0"
  ].join("\n");
}

/** Delete the rendered A/B wavs (the app calls this when it goes away). */
function w25AbCleanupScript() {
  return [
    "set -u",
    APPBASE_PRELUDE,
    'rm -f "' + PAYLOAD_TESTS + '"/' + AB_PREFIX + '*.wav 2>/dev/null',
    'echo OK',
    "exit 0"
  ].join("\n");
}

/** Parse "<tag>_*" keys out of the A/B script output into a variant object. */
function abVariant(kv, tag, name, label) {
  var bytes = parseInt(kv[tag + "_BYTES"], 10);
  if (!isFinite(bytes)) bytes = 0;
  function num(v) { var n = parseFloat(v); return isFinite(n) ? n : null; }
  return {
    label: label,
    file: name,
    url: AB_REL_URL + name,
    bytes: bytes,
    rendered: parseInt(kv[tag + "_RC"], 10) === 0 && bytes >= AB_MIN_BYTES,
    props: kv[tag + "_PROPS"] || "",
    meanDb: num(kv[tag + "_MEAN"]),
    peakDb: num(kv[tag + "_PEAK"])
  };
}

/* =======================================================================
 * CX mechanism shell builders  (mirror repo-root install.sh / init_dts.sh)
 * ===================================================================== */

/** The boot-time CX init script (mirrors init_dts.sh; constants baked in). */
function cxInitScriptBody() {
  var libLoop = [
    'for lib in ' + CX_GST_LIBS.join(" ") + '; do',
    '  src="' + CX_GST + '/$lib"',
    '  target="' + CX_GST_TARGET + '/$lib"',
    '  [ -f "$src" ] || continue',
    '  if grep -q " $target " /proc/mounts 2>/dev/null; then log "$lib already overridden";',
    '  else log "overriding $target"; mount -n --bind -o ro "$src" "$target" || log "WARN: bind mount failed for $lib"; fi',
    'done'
  ].join("\n");

  return [
    "#!/bin/sh",
    "# CX DTS-restore boot init (mirrors repo-root init_dts.sh).",
    "# Generated by DTS Enabler (io.github.josippapez.dtsenabler). Do not edit by hand.",
    "# Original library set never modified -> fully reversible. Always exits 0.",
    "set -u",
    "LOG=" + LOG,
    'log() { echo "[dts_restore-cx $*]" >> "$LOG" 2>/dev/null; }',
    "BAKED_GST_REGISTRY=\"\"",
    '[ -f "' + CX_ENV_CONF + '" ] && . "' + CX_ENV_CONF + '"',
    "# 1) Override the GStreamer plugins LG nerfed (guarded against double-mount).",
    libLoop,
    "# 2) Refresh the GStreamer registry (regenerated in /tmp, bound over the real path).",
    'REG="${GST_REGISTRY_1_0:-$BAKED_GST_REGISTRY}"',
    'if [ -n "$REG" ] && [ -f "$REG" ]; then',
    '  if grep -q " $REG " /proc/mounts 2>/dev/null; then log "registry already overridden";',
    '  else',
    '    log "refreshing GStreamer registry"',
    '    export GST_REGISTRY_1_0=/tmp/gst_1_0_registry.arm.bin',
    '    /usr/bin/gst-inspect-1.0 > /var/tmp/gst-inspect.log 2>&1',
    '    chmod 644 "$GST_REGISTRY_1_0" 2>/dev/null',
    '    chown :compositor "$GST_REGISTRY_1_0" 2>/dev/null',
    '    mount -n --bind "$GST_REGISTRY_1_0" "$REG" || log "WARN: registry bind failed"',
    '  fi',
    'fi',
    "# 3) Raise avdec_dca priority + apply the stereo downmix coefficients.",
    'if [ ! -f "' + GSTCOOL_TMP + '" ] && [ -f "' + GSTCOOL + '" ]; then',
    '  log "overriding ' + GSTCOOL + '"',
    '  sed "s/avdec_dca=0/avdec_dca=' + CX_DCA_RANK + '/" "' + GSTCOOL + '" > "' + GSTCOOL_TMP + '"',
    '  {',
    '    echo ""',
    '    echo "[downmix]"',
    '    echo "front=' + CX_DOWNMIX.front + '"',
    '    echo "center=' + CX_DOWNMIX.center + '"',
    '    echo "lfe=' + CX_DOWNMIX.lfe + '"',
    '    echo "rear=' + CX_DOWNMIX.rear + '"',
    '    echo "rear2=' + CX_DOWNMIX.rear2 + '"',
    '  } >> "' + GSTCOOL_TMP + '"',
    '  mount -n --bind "' + GSTCOOL_TMP + '" "' + GSTCOOL + '" || log "WARN: gstcool bind failed"',
    'fi',
    "# A non-zero webosbrew init script trips the failsafe that disables ALL root",
    "# customisations on the next boot, so always succeed.",
    "exit 0"
  ].join("\n");
}

/** enable (CX): stage payload, bake registry env, write init script, hook, apply. */
function cxEnable() {
  var b64 = Buffer.from(cxInitScriptBody(), "utf8").toString("base64");
  return [
    "set -u",
    APPBASE_PRELUDE,
    'LOG=' + LOG,
    'log() { echo "[dts_restore-cx-install $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    'log "=== enable (cx) start ==="',
    // 1. Stage the .so payload from the app.
    'mkdir -p "' + CX_GST + '" || { log "FATAL: cannot create ' + CX_GST + '"; exit 0; }',
    'n=0; for f in "' + PAYLOAD_CX + '"/*.so; do [ -f "$f" ] && cp -f "$f" "' + CX_GST + '/" && n=$((n+1)); done',
    'log "staged $n .so from ' + PAYLOAD_CX + '"',
    // 2. Bake the GStreamer registry path from THIS exec session (boot has none).
    'mkdir -p "' + CX_STATE + '"',
    'if [ -n "${GST_REGISTRY_1_0:-}" ] && [ -f "${GST_REGISTRY_1_0:-}" ]; then',
    '  printf "BAKED_GST_REGISTRY=\\"%s\\"\\n" "$GST_REGISTRY_1_0" > "' + CX_ENV_CONF + '" && log "baked registry $GST_REGISTRY_1_0"',
    'else log "WARN: GST_REGISTRY_1_0 not in exec env; boot will rely on init-time refresh only"; : > "' + CX_ENV_CONF + '"; fi',
    // 3. Write the init script (base64 heredoc).
    'base64 -d > "' + CX_INIT_SCRIPT + '" <<\'B64EOF\'',
    b64,
    "B64EOF",
    'chmod 0755 "' + CX_INIT_SCRIPT + '" && log "wrote ' + CX_INIT_SCRIPT + '"',
    // 4. Boot stub (exec our init script) + apply now.
    'mkdir -p "$(dirname "' + CX_HOOK + '")"',
    'if [ -L "' + CX_HOOK + '" ] || [ -e "' + CX_HOOK + '" ]; then rm -f "' + CX_HOOK + '"; fi',
    'printf "#!/bin/sh\\nexec %s\\n" "' + CX_INIT_SCRIPT + '" > "' + CX_HOOK + '" && chmod 0755 "' + CX_HOOK + '" && log "installed boot hook ' + CX_HOOK + '"',
    'sh "' + CX_INIT_SCRIPT + '"',
    'log "=== enable (cx) done ==="',
    'echo OK',
    "exit 0"
  ].join("\n");
}

/** disable (CX): remove boot hook + best-effort unmount. Original libs untouched. */
function cxDisable() {
  var lines = [
    "set +e",
    'LOG=' + LOG,
    'log() { echo "[dts_restore-cx-disable $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    // Same umount -> umount -l fallback the webOS 25 path uses. The "target is
    // busy because WebAppMgr has the .so mapped" evidence was measured on a C5,
    // not on CX hardware, so this is consistency rather than a proven CX bug --
    // but a plain umount that fails here would leave the override applied while
    // Disable reported success, which is the hazard either way.
    'cx_umount() {',
    '  grep -q " $1 " /proc/mounts 2>/dev/null || return 0',
    '  if umount "$1" 2>/dev/null; then log "unmounted $1"; return 0; fi',
    '  if umount -l "$1" 2>/dev/null; then log "lazy-detached busy bind $1 (a live mapping held it)"; return 0; fi',
    '  log "WARN could not unmount $1, even lazily"',
    '  return 1',
    '}',
    'log "=== disable (cx) start ==="',
    'if [ -e "' + CX_HOOK + '" ] || [ -L "' + CX_HOOK + '" ]; then rm -f "' + CX_HOOK + '" && log "removed boot hook"; else log "boot hook not present"; fi'
  ];
  CX_GST_LIBS.forEach(function (lib) {
    var dst = CX_GST_TARGET + "/" + lib;
    lines.push('cx_umount "' + dst + '"');
  });
  lines.push('cx_umount "' + GSTCOOL + '"');
  lines.push('rm -f "' + GSTCOOL_TMP + '" 2>/dev/null');
  // Unmount registry bind using the baked path, if any.
  lines.push('if [ -f "' + CX_ENV_CONF + '" ]; then . "' + CX_ENV_CONF + '"; if [ -n "${BAKED_GST_REGISTRY:-}" ]; then cx_umount "$BAKED_GST_REGISTRY"; fi; fi');
  lines.push('log "=== disable (cx) done ==="');
  lines.push('echo OK');
  return lines.join("\n");
}

/** uninstall (CX): disable + remove state dir. */
function cxUninstall() {
  return [
    cxDisable().replace(/\necho OK$/, ""),
    'rm -rf "' + CX_STATE + '" && echo "[dts_restore-cx-uninstall] removed ' + CX_STATE + '" >> "' + LOG + '" 2>&1',
    'echo OK'
  ].join("\n");
}

/* =======================================================================
 * Per-profile active-state probe (used by status)
 * ===================================================================== */
function w25StatusProbe() {
  return [
    'echo "HOOK=$([ -e ' + W25_HOOK + ' ] && echo 1 || echo 0)"',
    'echo "REGBIND=$(grep -c " ' + W25_REG_TARGET + ' " /proc/mounts 2>/dev/null)"',
    'echo "CFGBIND=$(grep -c " ' + W25_CFG_LIVE + ' " /proc/mounts 2>/dev/null)"',
    'echo "GCBIND=$(grep -c " ' + W25_GC_LIVE + ' " /proc/mounts 2>/dev/null)"',
    'echo "LIBAVBIND=$(grep -c " ' + W25_LGLIBAV + ' " /proc/mounts 2>/dev/null)"',
    'echo "DTSDEC=$(gst-inspect-1.0 dtsdec >/dev/null 2>&1 && echo 1 || echo 0)"',
    'echo "TRUEHD=$(gst-inspect-1.0 avdec_truehd >/dev/null 2>&1 && echo 1 || echo 0)"',
    'echo "DTSLIBSTAGED=$([ -f ' + W25_DEST + '/libgstdtsdec.so ] && echo 1 || echo 0)"',
    'echo "THDLIBSTAGED=$([ -f ' + W25_THD_DEST + '/libgstlibav.so ] && echo 1 || echo 0)"',
    'echo "ISOBIND=$(grep -c " ' + W25_ISO_LIVE + ' " /proc/mounts 2>/dev/null)"',
    'echo "TSDBIND=$(grep -c " ' + W25_TSD_LIVE + ' " /proc/mounts 2>/dev/null)"',
    'echo "DMXSTAGED=$([ -f ' + W25_DMX_DEST + '/libgstisomp4.so ] && [ -f ' + W25_DMX_DEST + '/libgstmpegtsdemux.so ] && echo 1 || echo 0)"'
  ].join("\n");
}
function cxStatusProbe() {
  return [
    'echo "HOOK=$([ -e ' + CX_HOOK + ' ] && echo 1 || echo 0)"',
    'echo "MOUNTS=$(grep -c gstreamer-1.0 /proc/mounts 2>/dev/null)"',
    'echo "RANK=$(grep -oE \'avdec_dca=[0-9]+\' ' + GSTCOOL + ' 2>/dev/null | head -1 | cut -d= -f2)"',
    'echo "LIBSTAGED=$([ -d ' + CX_GST + ' ] && echo 1 || echo 0)"'
  ].join("\n");
}

/* =======================================================================
 * Luna methods
 * ===================================================================== */

/* detect: run the read-only probe, return profile + compatibility verdict + raw
 * probes. `supported` keeps its old meaning ("a mechanism exists for this
 * profile"); `verdict` is the new, separate answer to "may we apply it HERE?". */
service.register("detect", function (message) {
  detectProfile().then(function (d) {
    var res = {
      returnValue: true,
      profile: d.profile,
      supported: isKnownProfile(d.profile),
      probes: d.probes
    };
    // Absent, not null: compatVerdict() returns null where the gate does not
    // apply (CX), and a present-but-null `verdict` would still defeat app.js's
    // `s.verdict || ...` fallback.
    addCompatFields(res, compatVerdict(d.profile, d.probes || {}));
    addHookFields(res, hookStaleness(d.profile, d.probes || {}, w25ExpectedInitMd5()));
    addPayloadFields(res, payloadStaleness(d.profile, d.probes || {}));
    message.respond(res);
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* status: detect, then check whether the matched mechanism is currently active. */
service.register("status", function (message) {
  detectProfile().then(function (d) {
    var profile = d.profile;
    var p = d.probes || {};
    var c = compatVerdict(profile, p);
    var base = {
      returnValue: true,
      profile: profile,
      supported: isKnownProfile(profile),
      model: p.PRODUCT_ID || "unknown",
      webosVersion: p.WEBOS_RELEASE || "unknown",
      gstVersion: p.GST_VERSION || "unknown",
      floatAbi: p.FLOAT_ABI || "unknown",
      loader: p.LOADER || "unknown",
      disableMechanism: p.DTS_DISABLE_MECHANISM_GUESS || "unknown",
      probes: p
    };
    addCompatFields(base, c);
    addHookFields(base, hookStaleness(profile, p, w25ExpectedInitMd5()));
    addPayloadFields(base, payloadStaleness(profile, p));

    if (profile === PROFILE_W25) {
      return rootExec(w25StatusProbe()).then(function (r) {
        var kv = parseKv(r.stdout);
        var hook = kv.HOOK === "1";
        var regbind = parseInt(kv.REGBIND, 10) > 0;
        var cfgbind = parseInt(kv.CFGBIND, 10) > 0;
        var gcbind = parseInt(kv.GCBIND, 10) > 0;
        var libavbind = parseInt(kv.LIBAVBIND, 10) > 0;
        var dtsdec = kv.DTSDEC === "1";
        var truehd = kv.TRUEHD === "1";
        base.mechanism = "decoder-inject (DTS + TrueHD/MLP)";
        base.hookInstalled = hook;
        base.registryBound = regbind;
        base.capabilityBound = cfgbind;
        base.gstcoolBound = gcbind;
        base.libavBound = libavbind;
        base.dtsdecPresent = dtsdec;
        base.truehdPresent = truehd;
        base.dtsPayloadStaged = kv.DTSLIBSTAGED === "1";
        base.truehdPayloadStaged = kv.THDLIBSTAGED === "1";
        var isobind = parseInt(kv.ISOBIND, 10) > 0;
        var tsdbind = parseInt(kv.TSDBIND, 10) > 0;
        base.isomp4Bound = isobind;
        base.mpegtsBound = tsdbind;
        base.demuxPayloadStaged = kv.DMXSTAGED === "1";
        base.containersActive = hook && isobind && tsdbind;   // mp4/ts/m2ts DTS
        // The registry is regenerated + COPIED over the media path (not bind-mounted),
        // so registryBound is expected false. `dtsdecPresent` (gst-inspect finds dtsdec)
        // is the authoritative signal that the live registry carries the DTS decoder.
        base.dtsActive = hook && dtsdec;
        base.truehdActive = hook && libavbind && cfgbind && gcbind && truehd;
        base.active = base.dtsActive && base.truehdActive;
        // "verified" now means THIS TV, not the mechanism: both codecs are proven
        // on a real C5 (decode + autoplug), but that only transfers to a TV whose
        // stock plugin fingerprints match the verified-sets table. Saying "yes"
        // on a forced/unverified TV would contradict the verdict beside it.
        base.verified = c.verdict === "verified";
        message.respond(base);
      });
    }

    if (profile === PROFILE_CX) {
      return rootExec(cxStatusProbe()).then(function (r) {
        var kv = parseKv(r.stdout);
        var hook = kv.HOOK === "1";
        var mounts = parseInt(kv.MOUNTS, 10);
        var rank = kv.RANK ? parseInt(kv.RANK, 10) : null;
        base.mechanism = "demuxer-override";
        base.hookInstalled = hook;
        base.overridesMounted = isFinite(mounts) && mounts > 0;
        base.mountCount = isFinite(mounts) ? mounts : 0;
        base.avdecDcaRank = (rank != null && isFinite(rank)) ? rank : null;
        base.payloadStaged = kv.LIBSTAGED === "1";
        base.active = hook && base.overridesMounted && rank === parseInt(CX_DCA_RANK, 10);
        base.verified = false;  // CX mechanism carried over, NOT verified on hardware
        message.respond(base);
      });
    }

    // Unknown / unsupported profile: report, never claim active.
    base.mechanism = "none";
    base.active = false;
    base.verified = false;
    message.respond(base);
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* enable: detect, branch to the matched mechanism, refuse on unknown. */
service.register("enable", function (message) {
  runMechanism(message, "enable");
});

/* disable: detect, branch, refuse on unknown. */
service.register("disable", function (message) {
  runMechanism(message, "disable");
});

/* uninstall: detect, branch, refuse on unknown. */
service.register("uninstall", function (message) {
  runMechanism(message, "uninstall");
});

/* test: decode each bundled DTS sample through the media registry and report
 * PASS/FAIL per container. Only defined for the webOS 25 profile. */
service.register("test", function (message) {
  detectProfile().then(function (d) {
    if (d.profile !== PROFILE_W25) {
      message.respond({
        returnValue: false, profile: d.profile, supported: false,
        errorText: "Self-test is only available on the webOS 25 profile (found '" + d.profile + "')."
      });
      return;
    }
    return rootExec(w25SelfTest()).then(function (r) {
      var kv = parseKv(r.stdout);
      var results = {};
      var allPass = true, anyRun = false;
      TEST_CASES.forEach(function (t) {
        var raw = kv[t.key] || "MISSING:0";
        var verdict = raw.split(":")[0];
        var bytes = parseInt((raw.split(":")[1] || "0"), 10) || 0;
        results[t.key] = { verdict: verdict, bytes: bytes, file: t.file };
        if (verdict === "PASS") anyRun = true; else if (verdict === "FAIL") { anyRun = true; allPass = false; }
      });
      message.respond({
        returnValue: true,
        profile: d.profile,
        dtsdecPresent: kv.DTSDEC === "1",
        results: results,
        pass: anyRun && allPass,
        summary: anyRun ? (allPass ? "All containers decode DTS — patch is working."
                                   : "Some containers failed to decode — patch not fully active.")
                        : "No test samples found (payload/testfiles not bundled)."
      });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* testfiles: return the on-device paths of the bundled samples so the UI can
 * play them by ear in an in-app <video>. Read-only, no privilege needed. */
service.register("testfiles", function (message) {
  var files = TEST_CASES.map(function (t) {
    return { key: t.key, file: t.file, path: PAYLOAD_TESTS + "/" + t.file };
  });
  message.respond({ returnValue: true, dir: PAYLOAD_TESTS, files: files });
});

/* setMakeupGain: write BOTH gain.conf files (DTS + TrueHD/MLP) via rootExec --
 * now the full per-codec config: the bare-float gain line PLUS the DRC
 * preset (drc/drc_boost/drc_cut) and center-boost, per the EPIC config
 * contract. Only meaningful on the webOS 25 profile (the only profile with
 * a make-up-gain-aware decoder); refuse cleanly elsewhere. Clamps gain and
 * center to their contract ranges and validates the preset against the
 * fixed enum before ANY of it touches rootExec -- so only our own
 * server-clamped numbers and one of the fixed PRESET_MAP mode strings ever
 * reach the shell. presetDts/presetThd/centerDts/centerThd default to
 * "off"/0 when omitted (older callers sending only {dts,truehd} still
 * work); when present they must be valid or the whole call is rejected,
 * same as the existing gain check. No registry re-init needed -- applies
 * on the next playback. */
service.register("setMakeupGain", function (message) {
  var p = message.payload || {};
  var dts = clampGainDb(p.dts);
  var thd = clampGainDb(p.truehd);
  if (dts === null || thd === null) {
    message.respond({
      returnValue: false,
      errorText: "Gain must be a finite number in [-20, 20] dB (got dts=" + p.dts + ", truehd=" + p.truehd + ")."
    });
    return;
  }
  var presetDts = (p.presetDts === undefined) ? "off" : normalizePreset(p.presetDts);
  var presetThd = (p.presetThd === undefined) ? "off" : normalizePreset(p.presetThd);
  if (presetDts === null || presetThd === null) {
    message.respond({
      returnValue: false,
      errorText: "DRC preset must be one of " + PRESET_ORDER.join("/") + " (got presetDts=" + p.presetDts + ", presetThd=" + p.presetThd + ")."
    });
    return;
  }
  var centerDts = (p.centerDts === undefined) ? 0 : clampCenterDb(p.centerDts);
  var centerThd = (p.centerThd === undefined) ? 0 : clampCenterDb(p.centerThd);
  if (centerDts === null || centerThd === null) {
    message.respond({
      returnValue: false,
      errorText: "Center boost must be a finite number in [-10, 10] dB (got centerDts=" + p.centerDts + ", centerThd=" + p.centerThd + ")."
    });
    return;
  }
  detectProfile().then(function (d) {
    if (d.profile !== PROFILE_W25) {
      message.respond({
        returnValue: false, profile: d.profile,
        errorText: "Make-up gain is only available on the webOS 25 profile (found '" + d.profile + "')."
      });
      return;
    }
    return rootExec(w25SetGainScript(dts, presetDts, centerDts, thd, presetThd, centerThd)).then(function (r) {
      if ((r.stdout || "").indexOf("OK") === -1) {
        message.respond({ returnValue: false, errorText: "Failed to write gain config: " + (r.stdout || r.stderr || "unknown error") });
        return;
      }
      message.respond({
        returnValue: true,
        dts: dts, truehd: thd,
        presetDts: presetDts, presetThd: presetThd,
        centerDts: centerDts, centerThd: centerThd
      });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* getMakeupGain: read both gain.conf files back so the UI can reflect the
 * current gain, DRC preset and center boost. Missing/empty/unparseable ->
 * the contract defaults (drc=off, boost/cut=100, center=0), mirroring the
 * decoders' own fault-tolerant parsing; never fails on a bad/missing file.
 * The preset is derived from (drc, drc_boost, drc_cut) via presetFromConfig
 * since the config contract has no separate "preset" key. */
service.register("getMakeupGain", function (message) {
  detectProfile().then(function (d) {
    if (d.profile !== PROFILE_W25) {
      message.respond({
        returnValue: true, profile: d.profile,
        dts: 0, truehd: 0, presetDts: "off", presetThd: "off", centerDts: 0, centerThd: 0
      });
      return;
    }
    return rootExec(w25GetGainScript()).then(function (r) {
      var saved = parseSavedConfig(parseKv(r.stdout));
      message.respond({
        returnValue: true,
        profile: d.profile,
        dts: saved.dts.gain,
        truehd: saved.truehd.gain,
        presetDts: saved.dts.preset,
        presetThd: saved.truehd.preset,
        centerDts: saved.dts.center,
        centerThd: saved.truehd.center
      });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* abPreview: render the bundled DTS sample twice -- A with the DRC/gain
 * path fully inert, B with the user's SAVED settings -- measure both, and
 * return the paths the UI can play plus the measured dB delta. Takes no
 * parameters: B is read from the on-disk config, never from the caller,
 * so this method cannot be used to push arbitrary values at the decoder.
 * webOS 25 only (the only profile with a DRC-aware dtsdec). */
service.register("abPreview", function (message) {
  detectProfile().then(function (d) {
    if (d.profile !== PROFILE_W25) {
      message.respond({
        returnValue: false, profile: d.profile,
        errorText: "A/B compare is only available on the webOS 25 profile (found '" + d.profile + "')."
      });
      return;
    }
    return rootExec(w25GetGainScript()).then(function (g) {
      var saved = parseSavedConfig(parseKv(g.stdout)).dts;
      // Fresh basenames per render: the UI can then point the player at a
      // distinct URL without a query string (which the platform pipeline
      // would treat as part of the filename -- see AB_PREFIX above).
      var stamp = Date.now().toString(36);
      var nameA = AB_PREFIX + "a_" + stamp + ".wav";
      var nameB = AB_PREFIX + "b_" + stamp + ".wav";
      return rootExec(w25AbScript(saved, nameA, nameB)).then(function (r) {
        var kv = parseKv(r.stdout);
        if (kv.ERR === "sample-missing") {
          message.respond({
            returnValue: false,
            errorText: "Bundled DTS sample " + AB_SAMPLE + " not found in " + (kv.DIR || "the app's payload") + "."
          });
          return;
        }
        if (kv.ERR === "render-dir-readonly") {
          message.respond({
            returnValue: false,
            errorText: "Cannot write the rendered clips to " + (kv.DIR || "the app directory") +
              " (read-only install). A/B compare needs a writable app directory."
          });
          return;
        }
        if (kv.ERR) {
          message.respond({ returnValue: false, errorText: "A/B render failed: " + kv.ERR });
          return;
        }

        var a = abVariant(kv, "A", nameA, "A - DRC off, 0 dB gain, 0 dB dialogue");
        var b = abVariant(kv, "B", nameB, "B - your saved settings");
        var measured = a.meanDb !== null && b.meanDb !== null;
        var confUnchanged =
          kv.DTS_CONF_BEFORE === kv.DTS_CONF_AFTER && kv.THD_CONF_BEFORE === kv.THD_CONF_AFTER;
        var hashUsable = kv.DTS_CONF_BEFORE !== "nohash" && kv.THD_CONF_BEFORE !== "nohash";

        function delta(x, y) { return (x === null || y === null) ? null : Math.round((y - x) * 10) / 10; }

        message.respond({
          returnValue: true,
          profile: d.profile,
          codec: "DTS",
          sample: AB_SAMPLE,
          saved: saved,
          a: a,
          b: b,
          measured: measured,
          measureNote: measured ? null
            : (kv.LEVEL === "1"
                ? "level ran but reported no usable values"
                : "GStreamer's `level` element is not registered on this TV, so no dB numbers could be taken"),
          deltaMeanDb: delta(a.meanDb, b.meanDb),
          deltaPeakDb: delta(a.peakDb, b.peakDb),
          // Proof, not a claim: hashes of both gain.conf files taken before the
          // first render and after the last one. The A/B only ever passes dtsdec
          // properties, so these must match.
          configUnchanged: hashUsable && confUnchanged,
          configProof: hashUsable
            ? ("dts " + kv.DTS_CONF_BEFORE + " -> " + kv.DTS_CONF_AFTER +
               ", truehd " + kv.THD_CONF_BEFORE + " -> " + kv.THD_CONF_AFTER)
            : "no md5sum on this TV, could not fingerprint gain.conf"
        });
      });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* abCleanup: remove the rendered A/B wavs. Called by the UI when the app
 * goes away; abPreview also clears stale renders before it starts, so a
 * missed cleanup can never leave more than one pair behind. */
service.register("abCleanup", function (message) {
  detectProfile().then(function (d) {
    if (d.profile !== PROFILE_W25) {
      message.respond({ returnValue: true, profile: d.profile, removed: false });
      return;
    }
    return rootExec(w25AbCleanupScript()).then(function (r) {
      message.respond({ returnValue: true, profile: d.profile, removed: (r.stdout || "").indexOf("OK") !== -1 });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/**
 * Shared enable/disable/uninstall dispatcher. Detects the profile fresh
 * (never trusts the caller), maps profile+action to a hardcoded command
 * builder, and refuses cleanly on an unknown/unsupported profile.
 *
 * The only caller-settable input is `enable {force: true}`. It is compared with
 * `=== true`, is honoured only when the read-only probe already reported
 * COMPAT_CANFORCE=1 (and the loader gate passed), and never reaches the shell --
 * it selects between two author-constant script texts inside w25Enable. So the
 * "no caller-controlled shell input" property in the header still holds exactly.
 */
function runMechanism(message, action) {
  var forceRequested = (message.payload || {}).force === true;
  detectProfile().then(function (d) {
    var profile = d.profile;
    var compat = compatVerdict(profile, d.probes || {});
    var builder = null;

    if (profile === PROFILE_W25) {
      if (action === "enable") {
        // compat is never null on this profile -- compatVerdict() only returns
        // null for CX, which is handled in the branch below.
        var forced = forceRequested && compat.canForce;
        builder = function () { return w25Enable(forced); };
      } else {
        builder = { disable: w25Disable, uninstall: w25Uninstall }[action];
      }
    } else if (profile === PROFILE_CX) {
      builder = { enable: cxEnable, disable: cxDisable, uninstall: cxUninstall }[action];
    }

    if (!builder) {
      message.respond({
        returnValue: false,
        profile: profile,
        supported: false,
        errorText: "Refusing to " + action + " on unsupported profile '" + profile +
          "'. This TV does not match a verified DTS-restore mechanism (see MULTI-MODEL.md). " +
          "Applying a mismatched mechanism could break MKV/MP4 playback."
      });
      return;
    }

    return rootExec(builder()).then(function (r) {
      var kv = parseKv(r.stdout);
      // The webOS 25 builders print REFUSED=<verdict> + REASON=<text> instead of
      // OK when the compatibility gate stood them down. Nothing was bound in that
      // case, so report it as a failed call with the gate's own reason rather
      // than a success whose stdout happens to say otherwise.
      if (kv.REFUSED) {
        var why = kv.REASON || (compat ? compat.verdictReason : "the mechanism refused to apply on this TV");
        var refusal = {
          returnValue: false,
          profile: profile,
          supported: true,
          action: action,
          verdict: kv.REFUSED,
          verdictReason: why,
          canForce: kv.CANFORCE === "1",
          errorText: "Refusing to " + action + " on this TV: " + why,
          stdout: r.stdout,
          stderr: r.stderr
        };
        // Nothing was applied, but on a TV that WAS enabled the refusal also
        // reverted it (binds detached before the preflight, registry rebuilt), and
        // saying "nothing changed" there would be false.
        if (kv.STOOD_DOWN !== undefined) {
          refusal.stoodDown = kv.STOOD_DOWN === "1";
          // The gate's reasons are sentence fragments without trailing punctuation.
          if (!/[.!?]$/.test(refusal.errorText)) refusal.errorText += ".";
          refusal.errorText += refusal.stoodDown
            ? " This TV was previously enabled, so the overrides have been reverted to stock."
            : " Nothing of ours was applied on this TV, and nothing was changed.";
        }
        message.respond(refusal);
        return;
      }
      // The success shape stays exactly what it was for any profile without a
      // compatibility gate (CX), so that path is untouched by this feature.
      var res = {
        returnValue: true,
        profile: profile,
        action: action,
        stdout: r.stdout,
        stderr: r.stderr
      };
      // Anything that did not actually happen has to show up here. None of these
      // is fatal on its own -- the call did run -- but reporting a clean result
      // when the revert is incomplete is the exact silence this change removes.
      var warnings = [];
      if (kv.WARN_UNMOUNT) {
        res.unmountWarning = kv.WARN_UNMOUNT;
        warnings.push("Some overrides could not be detached and are still active: " +
          kv.WARN_UNMOUNT + ". Reboot the TV to clear them.");
      }
      if (kv.REG_REVERTED !== undefined) {
        res.registryReverted = kv.REG_REVERTED === "1";
        if (!res.registryReverted) {
          warnings.push("LG's plugin registry could NOT be rebuilt, so our decoder may still be " +
            "registered and DTS may keep decoding until it is. Try again, or reboot the TV.");
        }
      }
      if (kv.UNINSTALL_DEFERRED === "1") {
        res.uninstallDeferred = true;
        warnings.push("The staged files were kept on purpose: deleting them while the live " +
          "registry still references them would break audio in other apps. Run Uninstall again.");
      }
      if (warnings.length) res.warning = warnings.join(" ");
      if (compat) {
        res.forced = (action === "enable") ? (forceRequested && compat.canForce) : false;
        res.verdict = kv.VERDICT || compat.verdict;
        res.verifiedLabel = kv.LABEL || compat.verifiedLabel;
      }
      message.respond(res);
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
}

/* ---- small helper ---- */
function parseKv(stdout) {
  var kv = {};
  (stdout || "").split("\n").forEach(function (line) {
    var i = line.indexOf("=");
    if (i > 0) kv[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  });
  return kv;
}
