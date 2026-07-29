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
 *       All bind-mounts, fully reversible. Both codecs proven on a real LG C5.
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
 * ===================================================================== */

"use strict";

var Service = require("webos-service");

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
].join("\n");

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

/* =======================================================================
 * webOS 25 mechanism shell builders  (mirror webos25/install.sh)
 * ===================================================================== */

/**
 * The canonical boot/apply script (verbatim from webos25/init_dts25.sh, proven
 * on a real LG C5). It (1) binds our TRUEHD/MLP-enabled codec capability config,
 * (2) binds our libgstlibav (avdec_truehd/avdec_mlp) over LG's, (3) binds our
 * gstcool.conf (avdec_truehd/mlp=310 SW-rank lever), (4) regenerates the media
 * GStreamer registry (with dtsdec + avdec_truehd) and writes it to the media
 * registry path -- only if the regen actually contains the decoders. Idempotent.
 */
function w25InitScriptBody() {
  return [
    "#!/bin/sh",
    "# webOS25 DTS + TrueHD restore. Runs at boot via /var/lib/webosbrew/init.d/restore_dts25.",
    "# Generated by DTS Enabler (io.github.josippapez.dtsenabler) -- verbatim copy of the",
    "# proven webos25/init_dts25.sh. Do not edit by hand.",
    "set -u",
    "REG=/mnt/flash/data/gst_1_0_registry.arm.bin",
    "CFG=/etc/umediaserver/device_codec_capability_config.json",
    "LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so",
    "MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so",
    "LOG=/tmp/dts25.log",
    "EXPECT_GST=1.24",
    'toast() { luna-send -n 1 luna://com.webos.notification/createToast "{\\"sourceId\\":\\"io.github.josippapez.dtsenabler\\",\\"message\\":\\"$1\\"}" >/dev/null 2>&1; }',
    'echo "--- dts25+truehd $(date) ---" >> $LOG 2>&1',
    "# 0) firmware-update / ABI guard. Our bind-over libs (libav/isomp4/mpegtsdemux) are armel",
    "#    GStreamer-1.24 builds; binding them over a different-ABI LG lib after an OTA would break",
    "#    ALL mp4/ts/mkv playback. Detect via the untouched core version; if it changed, do NOTHING",
    "#    and let stock firmware play (losing only DTS/TrueHD) -- fail safe, never break playback.",
    'GST_VER=$(/usr/bin/gst-inspect-1.0 --version 2>/dev/null | sed -n \'s/^GStreamer \\([0-9]*\\.[0-9]*\\).*/\\1/p\' | head -n1)',
    'if [ "$GST_VER" != "$EXPECT_GST" ]; then',
    '  echo "ABORT: GStreamer \'$GST_VER\' != expected $EXPECT_GST (firmware update?); overrides skipped" >> $LOG',
    '  toast "DTS/TrueHD paused: TV firmware changed (GStreamer $GST_VER). Re-open DTS Enabler to update."',
    "  exit 0",
    "fi",
    "# 1) codec-capability override (adds TRUEHD/MLP so umediaserver allocates a decoder resource)",
    '[ -f /var/lib/webosbrew/truehd/codec_capability.json ] && ! grep -q " $CFG " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/codec_capability.json "$CFG" 2>>$LOG',
    "# 2) replace LG.s truehd-less libav with ours (has avdec_truehd/avdec_mlp)",
    '[ -f "$MYLIBAV" ] && ! grep -q " $LGLIBAV " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYLIBAV" "$LGLIBAV" 2>>$LOG',
    "# 2b) gstcool.conf: give avdec_truehd a high SW rank so LG autoplugs it (not the HW path)",
    "GC=/etc/gst/gstcool.conf",
    '[ -f /var/lib/webosbrew/truehd/gstcool.conf ] && ! grep -q " $GC " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/gstcool.conf "$GC" 2>>$LOG',
    "# 2c) container demuxers with DTS re-enabled (mp4/ts/m2ts DTS -> audio/x-dts).",
    "#     Patched isomp4/mpegtsdemux default dts_support=TRUE. Bound BEFORE the regen",
    "#     below so the registry picks them up at their normal path.",
    "ISO=/usr/lib/gstreamer-1.0/libgstisomp4.so",
    "TSD=/usr/lib/gstreamer-1.0/libgstmpegtsdemux.so",
    '[ -f /var/lib/webosbrew/demux25/libgstisomp4.so ] && ! grep -q " $ISO " /proc/mounts 2>/dev/null && mount -n --bind -o ro /var/lib/webosbrew/demux25/libgstisomp4.so "$ISO" 2>>$LOG',
    '[ -f /var/lib/webosbrew/demux25/libgstmpegtsdemux.so ] && ! grep -q " $TSD " /proc/mounts 2>/dev/null && mount -n --bind -o ro /var/lib/webosbrew/demux25/libgstmpegtsdemux.so "$TSD" 2>>$LOG',
    "# 3) regenerate the media registry (fresh) with dtsdec + our libav, then write it to the media path.",
    "#    Bounded by `timeout` and scanned in-process (GST_REGISTRY_FORK=no) so a hang can't trip HBC",
    "#    failsafe and no gst-plugin-scanner child lingers past the timeout.",
    "rm -f /tmp/gst_dts_reg.bin",
    "LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs \\",
    "GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin \\",
    "GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \\",
    "GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 30 /usr/bin/gst-inspect-1.0 >/dev/null 2>>$LOG",
    "# only overwrite the media registry if our regen actually contains BOTH decoders",
    "if GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 dtsdec >/dev/null 2>&1 \\",
    "   && GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin GST_REGISTRY_UPDATE=no GST_REGISTRY_FORK=no /usr/bin/gst-inspect-1.0 avdec_truehd >/dev/null 2>&1; then",
    '  cp -f /tmp/gst_dts_reg.bin "$REG" 2>>$LOG && echo "registry updated (dtsdec+truehd)" >>$LOG',
    "else",
    '  echo "WARN: regen missing dtsdec or avdec_truehd, left registry untouched" >>$LOG',
    '  toast "DTS Enabler: decoder registry incomplete after boot; DTS/TrueHD may not work."',
    "fi",
    "exit 0"
  ].join("\n");
}

/** enable (webOS 25): stage BOTH payloads, generate BOTH /etc overrides by
 *  editing the TV's own live files, install the canonical init script + hook,
 *  apply now, restart. Mirrors webos25/install.sh. */
function w25Enable() {
  var b64init = Buffer.from(w25InitScriptBody(), "utf8").toString("base64");
  var b64cap  = Buffer.from(W25_CAP_AWK, "utf8").toString("base64");
  var b64gc   = Buffer.from(W25_GC_AWK, "utf8").toString("base64");
  return [
    "set -u",
    APPBASE_PRELUDE,
    'LOG=' + LOG,
    'log() { echo "[dts25-install $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    'log "=== enable (webos25 DTS+TrueHD) start ==="',
    'log "app base: $APPBASE"',
    // 1. Stage the DTS payload.
    'mkdir -p "' + W25_LIBS + '" || { log "FATAL: cannot create ' + W25_LIBS + '"; exit 0; }',
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
    // 3. Unmount any stale binds so overrides are generated from PRISTINE /etc.
    'for T in "' + W25_CFG_LIVE + '" "' + W25_GC_LIVE + '" "' + W25_LGLIBAV + '" "' + W25_ISO_LIVE + '" "' + W25_TSD_LIVE + '" "' + W25_REG_TARGET + '"; do',
    '  if grep -q " $T " /proc/mounts 2>/dev/null; then umount "$T" 2>>"$LOG" && log "unmounted stale bind $T" || log "WARN: could not umount $T"; fi',
    'done',
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
    'mkdir -p "$(dirname "' + W25_HOOK + '")"',
    'if [ -L "' + W25_HOOK + '" ] || [ -e "' + W25_HOOK + '" ]; then rm -f "' + W25_HOOK + '"; fi',
    'ln -s "' + W25_INIT_SCRIPT + '" "' + W25_HOOK + '" && log "linked boot hook ' + W25_HOOK + '"',
    // 5. Apply now + restart the media pipeline.
    'sh "' + W25_INIT_SCRIPT + '"',
    'if killall starfish-media-pipeline 2>>"$LOG"; then log "restarted starfish-media-pipeline"; else log "note: media pipeline not running"; fi',
    'log "=== enable (webos25 DTS+TrueHD) done ==="',
    'echo OK',
    "exit 0"
  ].join("\n");
}

/** disable (webOS 25): remove boot hook, unmount all four binds. Keep staged libs. */
function w25Disable() {
  return [
    "set -u",
    'LOG=' + LOG,
    'log() { echo "[dts25-disable $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    'log "=== disable (webos25) start ==="',
    'if [ -L "' + W25_HOOK + '" ] || [ -e "' + W25_HOOK + '" ]; then rm -f "' + W25_HOOK + '" && log "removed boot hook"; else log "boot hook not present"; fi',
    'for T in "' + W25_CFG_LIVE + '" "' + W25_GC_LIVE + '" "' + W25_LGLIBAV + '" "' + W25_ISO_LIVE + '" "' + W25_TSD_LIVE + '" "' + W25_REG_TARGET + '"; do',
    '  if grep -q " $T " /proc/mounts 2>/dev/null; then umount "$T" 2>>"$LOG" && log "unmounted bind over $T (reverted)" || log "WARN could not umount $T"; else log "no bind over $T"; fi',
    'done',
    // The registry is written with `cp -f` (persistent), NOT bind-mounted -- so a
    // umount can never revert it (that was the bug: disable/uninstall left a stale
    // registry referencing our removed /var/lib/webosbrew libs, which breaks
    // media-pipeline app audio like Spotify until a valid registry is regenerated;
    // root-caused on a real C5, 2026-07-23). The binds above are already removed, so
    // regenerate a clean STOCK catalog from the pristine on-disk plugins.
    'CLEAN_REG=/tmp/gst_clean_reg.bin; rm -f "$CLEAN_REG" 2>/dev/null',
    'if GST_REGISTRY_1_0="$CLEAN_REG" GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0 GST_REGISTRY_FORK=no GST_REGISTRY_UPDATE=yes timeout 60 /usr/bin/gst-inspect-1.0 >/dev/null 2>>"$LOG"; then',
    '  cp -f "$CLEAN_REG" "' + W25_REG_TARGET + '" 2>>"$LOG" && log "regenerated clean stock registry (reverted cp-based override)" || log "WARN: could not write clean registry"',
    'else',
    '  log "WARN: clean registry regen failed; leaving existing registry untouched (may still be stale)"',
    'fi',
    'rm -f "$CLEAN_REG" 2>/dev/null',
    'rm -f "' + W25_REG_TMP + '" 2>/dev/null',
    'if killall starfish-media-pipeline 2>>"$LOG"; then log "restarted media pipeline"; else log "note: media pipeline not running"; fi',
    'log "=== disable (webos25) done ==="',
    'echo OK',
    "exit 0"
  ].join("\n");
}

/** uninstall (webOS 25): disable + remove both state dirs (dts25 + truehd). */
function w25Uninstall() {
  return [
    w25Disable().replace(/\necho OK\nexit 0$/, ""),
    'rm -rf "' + W25_DEST + '" "' + W25_THD_DEST + '" "' + W25_DMX_DEST + '" && echo "[dts25-uninstall] removed ' + W25_DEST + ' + ' + W25_THD_DEST + ' + ' + W25_DMX_DEST + '" >> "' + LOG + '" 2>&1',
    'echo OK',
    "exit 0"
  ].join("\n");
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
    'export GST_REGISTRY_1_0="$REG" GST_REGISTRY_UPDATE=no',
    'echo "DTSDEC=$(gst-inspect-1.0 dtsdec >/dev/null 2>&1 && echo 1 || echo 0)"'
  ];
  TEST_CASES.forEach(function (t) {
    var f = PAYLOAD_TESTS + "/" + t.file;
    lines.push('F="' + f + '"');
    lines.push('rm -f "$OUT"');
    lines.push('if [ -f "$F" ]; then');
    lines.push('  timeout 25 gst-launch-1.0 -q filesrc location="$F" ! ' + t.demux + ' name=d d. ! queue ! dtsdec ! audioconvert ! wavenc ! filesink location="$OUT" >/dev/null 2>&1');
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
    'log "=== disable (cx) start ==="',
    'if [ -e "' + CX_HOOK + '" ] || [ -L "' + CX_HOOK + '" ]; then rm -f "' + CX_HOOK + '" && log "removed boot hook"; else log "boot hook not present"; fi'
  ];
  CX_GST_LIBS.forEach(function (lib) {
    var dst = CX_GST_TARGET + "/" + lib;
    lines.push('grep -q " ' + dst + ' " /proc/mounts 2>/dev/null && umount "' + dst + '" 2>/dev/null && log "unmounted ' + lib + '"');
  });
  lines.push('grep -q " ' + GSTCOOL + ' " /proc/mounts 2>/dev/null && umount "' + GSTCOOL + '" 2>/dev/null && log "unmounted gstcool.conf"');
  lines.push('rm -f "' + GSTCOOL_TMP + '" 2>/dev/null');
  // Unmount registry bind using the baked path, if any.
  lines.push('if [ -f "' + CX_ENV_CONF + '" ]; then . "' + CX_ENV_CONF + '"; if [ -n "${BAKED_GST_REGISTRY:-}" ] && grep -q " $BAKED_GST_REGISTRY " /proc/mounts 2>/dev/null; then umount "$BAKED_GST_REGISTRY" 2>/dev/null && log "unmounted registry override"; fi; fi');
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

/* detect: run the read-only probe, return profile + raw probes. */
service.register("detect", function (message) {
  detectProfile().then(function (d) {
    message.respond({
      returnValue: true,
      profile: d.profile,
      supported: isKnownProfile(d.profile),
      probes: d.probes
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* status: detect, then check whether the matched mechanism is currently active. */
service.register("status", function (message) {
  detectProfile().then(function (d) {
    var profile = d.profile;
    var p = d.probes || {};
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
        base.verified = true;   // both codecs verified on a real C5 (decode + autoplug)
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
 */
function runMechanism(message, action) {
  detectProfile().then(function (d) {
    var profile = d.profile;
    var builder = null;

    if (profile === PROFILE_W25) {
      builder = { enable: w25Enable, disable: w25Disable, uninstall: w25Uninstall }[action];
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
      message.respond({
        returnValue: true,
        profile: profile,
        action: action,
        stdout: r.stdout,
        stderr: r.stderr
      });
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
