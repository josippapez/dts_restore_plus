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

var PKG_ID = "org.webosbrew.dtsenabler.service";
var service = new Service(PKG_ID);

var HBC_EXEC = "luna://org.webosbrew.hbchannel.service/exec";

/* ---- App install tree (payload ships under the APP, not the service) ---- */
// On webOS the app and its JS service install into SEPARATE trees, so the
// service cannot reach the payload via a path relative to __dirname. Address
// the application install dir explicitly.
var APP_ID       = "org.webosbrew.dtsenabler";
var APP_INSTALL  = "/usr/palm/applications/" + APP_ID;
var PAYLOAD_W25     = APP_INSTALL + "/payload/webos25";          // libgstdtsdec.so + libdca.so.0
var PAYLOAD_W25_THD = APP_INSTALL + "/payload/webos25-truehd";   // libgstlibav.so + ffmpeg libs
var PAYLOAD_CX      = APP_INSTALL + "/payload/cx";               // CX demuxer/libav .so set

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
      if (p.returnValue === false && p.stdout === undefined && p.stderr === undefined) {
        reject({
          errorText: p.errorText || "exec call failed - is the Homebrew Channel installed and the TV rooted?",
          raw: p
        });
        return;
      }
      resolve({
        stdout: p.stdout || "",
        stderr: p.stderr || "",
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
    "# Generated by DTS Enabler (org.webosbrew.dtsenabler) -- verbatim copy of the",
    "# proven webos25/init_dts25.sh. Do not edit by hand.",
    "set -u",
    "REG=/mnt/flash/data/gst_1_0_registry.arm.bin",
    "CFG=/etc/umediaserver/device_codec_capability_config.json",
    "LGLIBAV=/usr/lib/gstreamer-1.0/libgstlibav.so",
    "MYLIBAV=/var/lib/webosbrew/truehd/libgstlibav.so",
    "LOG=/tmp/dts25.log",
    'echo "--- dts25+truehd $(date) ---" >> $LOG 2>&1',
    "# 1) codec-capability override (adds TRUEHD/MLP so umediaserver allocates a decoder resource)",
    '[ -f /var/lib/webosbrew/truehd/codec_capability.json ] && ! grep -q " $CFG " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/codec_capability.json "$CFG" 2>>$LOG',
    "# 2) replace LG.s truehd-less libav with ours (has avdec_truehd/avdec_mlp)",
    '[ -f "$MYLIBAV" ] && ! grep -q " $LGLIBAV " /proc/mounts 2>/dev/null && mount -n --bind -o ro "$MYLIBAV" "$LGLIBAV" 2>>$LOG',
    "# 2b) gstcool.conf: give avdec_truehd a high SW rank so LG autoplugs it (not the HW path)",
    "GC=/etc/gst/gstcool.conf",
    '[ -f /var/lib/webosbrew/truehd/gstcool.conf ] && ! grep -q " $GC " /proc/mounts 2>/dev/null && mount -n --bind /var/lib/webosbrew/truehd/gstcool.conf "$GC" 2>>$LOG',
    "# 3) regenerate the media registry (fresh) with dtsdec + our libav, then write it to the media path",
    "rm -f /tmp/gst_dts_reg.bin",
    "LD_LIBRARY_PATH=/var/lib/webosbrew/truehd/libs \\",
    "GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin \\",
    "GST_PLUGIN_PATH_1_0=/usr/lib/gstreamer-1.0:/mnt/lg/res/lglib/gstreamer-1.0:/var/lib/webosbrew/dts25 \\",
    "GST_REGISTRY_UPDATE=yes /usr/bin/gst-inspect-1.0 >/dev/null 2>>$LOG",
    "# only overwrite the media registry if our regen actually contains the decoders",
    "if GST_REGISTRY_1_0=/tmp/gst_dts_reg.bin GST_REGISTRY_UPDATE=no /usr/bin/gst-inspect-1.0 avdec_truehd >/dev/null 2>&1; then",
    '  cp -f /tmp/gst_dts_reg.bin "$REG" 2>>$LOG && echo "registry updated (dtsdec+truehd)" >>$LOG',
    "else",
    '  echo "WARN: regen missing avdec_truehd, left registry untouched" >>$LOG',
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
    'LOG=' + LOG,
    'log() { echo "[dts25-install $(date \'+%Y-%m-%d %H:%M:%S\')] $*" >> "$LOG" 2>&1; }',
    'log "=== enable (webos25 DTS+TrueHD) start ==="',
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
    // 3. Unmount any stale binds so overrides are generated from PRISTINE /etc.
    'for T in "' + W25_CFG_LIVE + '" "' + W25_GC_LIVE + '" "' + W25_LGLIBAV + '" "' + W25_REG_TARGET + '"; do',
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
    'for T in "' + W25_CFG_LIVE + '" "' + W25_GC_LIVE + '" "' + W25_LGLIBAV + '" "' + W25_REG_TARGET + '"; do',
    '  if grep -q " $T " /proc/mounts 2>/dev/null; then umount "$T" 2>>"$LOG" && log "unmounted bind over $T (reverted)" || log "WARN could not umount $T"; else log "no bind over $T"; fi',
    'done',
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
    'rm -rf "' + W25_DEST + '" "' + W25_THD_DEST + '" && echo "[dts25-uninstall] removed ' + W25_DEST + ' + ' + W25_THD_DEST + '" >> "' + LOG + '" 2>&1',
    'echo OK',
    "exit 0"
  ].join("\n");
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
    "# Generated by DTS Enabler (org.webosbrew.dtsenabler). Do not edit by hand.",
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
    'echo "THDLIBSTAGED=$([ -f ' + W25_THD_DEST + '/libgstlibav.so ] && echo 1 || echo 0)"'
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
        base.dtsActive = hook && regbind && dtsdec;
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
