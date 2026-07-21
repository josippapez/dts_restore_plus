/* =====================================================================
 * DTS Enabler - privileged backend JS service
 * ---------------------------------------------------------------------
 * This service exposes friendly Luna methods (enable/disable/status/...)
 * to the DTS Enabler web app. It holds NO elevation of its own: every
 * privileged action is executed as root by shelling the command out to
 * the Homebrew Channel (HBC) exec service:
 *
 *     luna://org.webosbrew.hbchannel.service/exec  { "command": "<sh>" }
 *
 * That service runs on a rooted TV and returns { stdout, stderr,
 * returnValue }. The shell strings we build here mirror the hardened
 * upstream dts_restore init_dts.sh: idempotent, guarded bind-mounts,
 * logging to /tmp/dtsenabler.log.
 *
 * SECURITY MODEL
 *   - Everything we hand to exec runs as ROOT. Treat every value that
 *     ends up in a command string as hostile.
 *   - The only caller-controlled values are the 5 downmix coefficients
 *     and a DV boolean. Coefficients are forced through numeric
 *     validation (validateCoeff) that yields a fixed "d.dd" string
 *     containing only [0-9.]; anything else is rejected before it can
 *     reach a shell. The DV flag is coerced to a literal 0/1. No
 *     free-form strings are ever interpolated.
 *   - Paths we control are constants; we never interpolate untrusted
 *     path fragments.
 * ===================================================================== */

"use strict";

var Service = require("webos-service");
var path = require("path");

var PKG_ID = "org.webosbrew.dtsenabler.service";
var service = new Service(PKG_ID);

var HBC_EXEC = "luna://org.webosbrew.hbchannel.service/exec";

/* ---- Stable on-device paths -------------------------------------------- */

// Where this app is installed. The service runs from <app>/service.
var APP_ROOT = path.resolve(__dirname, "..");
// Vendored patched GStreamer plugins shipped inside the .ipk.
var PAYLOAD_GST = path.join(APP_ROOT, "vendor", "gst");

// Persistent, root-owned working area (survives reboots; readable by init.d).
var STATE_DIR   = "/var/lib/webosbrew/dtsenabler";
var STATE_GST   = STATE_DIR + "/gst";              // copy of the .so payload
var CONFIG_JSON = STATE_DIR + "/config.json";      // our persisted config
var DV_PERSIST  = STATE_DIR + "/dv_disabled";      // presence => DV off at boot

var INIT_DIR    = "/var/lib/webosbrew/init.d";
var INIT_SCRIPT = INIT_DIR + "/restore_dts";       // NOTE: no .sh extension
var LOG         = "/tmp/dtsenabler.log";

var GST_TARGET  = "/usr/lib/gstreamer-1.0";
var GSTCOOL     = "/etc/gst/gstcool.conf";
var GSTCOOL_TMP = "/tmp/gstcool.conf";

// The plugin .so set we bind-mount over the LG-nerfed originals.
var GST_LIBS = [
  "libgstisomp4.so",
  "libgstisomp4_1_8.so",
  "libgstmatroska.so",
  "libgstlibav.so"
];

var DOWNMIX_KEYS = ["front", "center", "lfe", "rear", "rear2"];
var DOWNMIX_DEFAULTS = { front: "1.25", center: "0.75", lfe: "0.75", rear: "0.75", rear2: "0.70" };
var COEFF_MIN = 0.0;
var COEFF_MAX = 2.0;

/* ======================================================================
 * Root exec helper
 * ====================================================================== */

/**
 * Run a shell command as root via the Homebrew Channel exec service.
 * @param {string} command  A complete /bin/sh command line.
 * @returns {Promise<{stdout:string, stderr:string, returnValue:boolean}>}
 *
 * The HBC exec service is one-shot: it runs the command, waits for it to
 * finish, and returns its captured output. We resolve on transport
 * success and let callers inspect stdout/stderr/returnValue; we reject
 * only when the Luna call itself fails (service missing => not rooted).
 */
function rootExec(command) {
  return new Promise(function (resolve, reject) {
    service.call(HBC_EXEC, { command: command }, function (msg) {
      var p = (msg && msg.payload) ? msg.payload : {};
      if (p.returnValue === false && p.stdout === undefined && p.stderr === undefined) {
        // Pure Luna failure (e.g. HBC not installed / not rooted).
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

/* ======================================================================
 * Input validation
 * ====================================================================== */

/**
 * Validate a single downmix coefficient.
 * @returns {string} normalised "d.dd" string (only [0-9.] characters)
 * @throws  {Error} if not a finite number in [COEFF_MIN, COEFF_MAX]
 */
function validateCoeff(raw) {
  var n = Number(raw);
  if (typeof raw === "boolean" || !isFinite(n)) {
    throw new Error("downmix coefficient is not a number: " + JSON.stringify(raw));
  }
  if (n < COEFF_MIN || n > COEFF_MAX) {
    throw new Error("downmix coefficient out of range [" + COEFF_MIN + "," + COEFF_MAX + "]: " + n);
  }
  var s = n.toFixed(2);
  // Belt-and-braces: the produced token must be shell-inert.
  if (!/^[0-9]+\.[0-9]{2}$/.test(s)) {
    throw new Error("downmix coefficient failed sanitisation: " + s);
  }
  return s;
}

/**
 * Validate a full downmix parameter object into a clean map of "d.dd"
 * strings. Missing keys fall back to defaults.
 */
function validateDownmix(params) {
  params = params || {};
  var out = {};
  DOWNMIX_KEYS.forEach(function (k) {
    out[k] = (params[k] === undefined || params[k] === null)
      ? DOWNMIX_DEFAULTS[k]
      : validateCoeff(params[k]);
  });
  return out;
}

/* ======================================================================
 * Shell fragment builders (all inputs pre-validated)
 * ====================================================================== */

/**
 * Build the body of the boot-persistence init script.
 * Mirrors upstream init_dts.sh but reads the .so payload from the stable
 * STATE_GST copy and embeds the current, validated downmix + DV config.
 */
function buildInitScript(downmix, dvDisabled) {
  var libLoop = GST_LIBS.map(function (lib) {
    return '  [ -f "' + STATE_GST + '/' + lib + '" ] && ' +
           'mount -n --bind -o ro "' + STATE_GST + '/' + lib + '" "' + GST_TARGET + '/' + lib + '" ' +
           '&& echo "bound ' + lib + '" >> "' + LOG + '"';
  }).join("\n");

  var downmixSection =
    "[downmix]\n" +
    "front=" + downmix.front + "\n" +
    "center=" + downmix.center + "\n" +
    "lfe=" + downmix.lfe + "\n" +
    "rear=" + downmix.rear + "\n" +
    "rear2=" + downmix.rear2 + "\n";

  // NOTE: this whole string is written to disk via a single-quoted heredoc
  // (see writeInitScript), so $VARS below are evaluated at BOOT, not now.
  return [
    "#!/bin/bash",
    "# Generated by DTS Enabler (org.webosbrew.dtsenabler). Do not edit by hand.",
    'echo "--- restore_dts $(date) ---" >> "' + LOG + '"',
    "",
    "# 1) Bind-mount the patched GStreamer plugins over LG's nerfed originals.",
    libLoop,
    "",
    "# 2) Rebuild + override the GStreamer registry so the new ranks take effect.",
    "#    ORIG_REG captures the real registry path BEFORE we repoint the var.",
    'if [ -n "$GST_REGISTRY_1_0" ] && [ -f "$GST_REGISTRY_1_0" ]; then',
    '  ORIG_REG="$GST_REGISTRY_1_0"',
    '  export GST_REGISTRY_1_0=/tmp/gst_1_0_registry.arm.bin',
    '  /usr/bin/gst-inspect-1.0 > /var/tmp/gst-inspect.log 2>&1',
    '  chmod 666 "$GST_REGISTRY_1_0" 2>/dev/null',
    '  chown :compositor "$GST_REGISTRY_1_0" 2>/dev/null',
    '  mount -n --bind "$GST_REGISTRY_1_0" "$ORIG_REG" 2>/dev/null',
    'fi',
    "",
    "# 3) Rewrite gstcool.conf: enable avdec_dca (rank 290) + downmix section.",
    'if [ ! -f "' + GSTCOOL_TMP + '" ] && [ -f "' + GSTCOOL + '" ]; then',
    '  sed "s/avdec_dca=0/avdec_dca=290/" "' + GSTCOOL + '" > "' + GSTCOOL_TMP + '"',
    '  cat >> "' + GSTCOOL_TMP + '" <<\'DMIX\'',
    "",
    downmixSection.replace(/\n$/, ""),
    "DMIX",
    '  mount -n --bind "' + GSTCOOL_TMP + '" "' + GSTCOOL + '" && echo "bound gstcool.conf" >> "' + LOG + '"',
    'fi',
    "",
    "# 4) Dolby Vision persistence: recreate /tmp/dv_disable at boot if configured.",
    'if [ -f "' + DV_PERSIST + '" ]; then',
    '  : > /tmp/dv_disable',
    '  echo "DV disabled at boot" >> "' + LOG + '"',
    'fi'
  ].join("\n");
}

/**
 * Command that (re)writes the init script atomically to disk.
 * We pipe our generated content through a single-quoted heredoc so nothing
 * inside expands during the write; the content itself is already sanitised.
 */
function cmdWriteInitScript(scriptBody) {
  // Base64-encode the body so we never worry about heredoc terminators or
  // shell metacharacters surviving the write. `base64 -d` reconstitutes it.
  var b64 = Buffer.from(scriptBody, "utf8").toString("base64");
  return [
    "set -e",
    'mkdir -p "' + INIT_DIR + '"',
    // Split base64 into a heredoc to stay well under ARG_MAX.
    'base64 -d > "' + INIT_SCRIPT + '" <<\'B64EOF\'',
    b64,
    "B64EOF",
    'chmod 755 "' + INIT_SCRIPT + '"'
  ].join("\n");
}

/** Command that stages the .so payload into the stable STATE_GST dir. */
function cmdStagePayload() {
  return [
    'mkdir -p "' + STATE_GST + '"',
    // Copy every shipped .so; -f to overwrite on app update.
    'for f in "' + PAYLOAD_GST + '"/*.so; do [ -f "$f" ] && cp -f "$f" "' + STATE_GST + '/"; done',
    'echo "staged payload" >> "' + LOG + '"'
  ].join("\n");
}

/** Command that persists our config.json (validated values only). */
function cmdWriteConfig(downmix, dvDisabled) {
  var cfg = {
    downmix: downmix,
    dvDisabled: !!dvDisabled,
    version: 1
  };
  var b64 = Buffer.from(JSON.stringify(cfg, null, 2), "utf8").toString("base64");
  return [
    'mkdir -p "' + STATE_DIR + '"',
    'base64 -d > "' + CONFIG_JSON + '" <<\'B64EOF\'',
    b64,
    "B64EOF"
  ].join("\n");
}

/**
 * Command that applies (or re-applies) gstcool.conf immediately, without a
 * reboot: unbind any existing override, regenerate from the validated
 * downmix, and bind-mount it back.
 */
function cmdApplyGstcool(downmix) {
  var downmixSection =
    "[downmix]\\n" +
    "front=" + downmix.front + "\\n" +
    "center=" + downmix.center + "\\n" +
    "lfe=" + downmix.lfe + "\\n" +
    "rear=" + downmix.rear + "\\n" +
    "rear2=" + downmix.rear2 + "\\n";

  return [
    "set -e",
    "# Idempotent: drop a previous bind-mount if present.",
    'if grep -q "' + GSTCOOL + '" /proc/mounts 2>/dev/null; then umount "' + GSTCOOL + '" 2>/dev/null || true; fi',
    'rm -f "' + GSTCOOL_TMP + '"',
    'if [ -f "' + GSTCOOL + '" ]; then',
    '  sed "s/avdec_dca=0/avdec_dca=290/" "' + GSTCOOL + '" > "' + GSTCOOL_TMP + '"',
    '  printf "\\n' + downmixSection + '" >> "' + GSTCOOL_TMP + '"',
    '  mount -n --bind "' + GSTCOOL_TMP + '" "' + GSTCOOL + '"',
    '  echo "applied gstcool.conf" >> "' + LOG + '"',
    'fi'
  ].join("\n");
}

/** Command that applies the .so bind-mounts immediately (idempotent). */
function cmdApplyMounts() {
  var lines = ["set +e"];
  GST_LIBS.forEach(function (lib) {
    var src = STATE_GST + "/" + lib;
    var dst = GST_TARGET + "/" + lib;
    lines.push(
      'if [ -f "' + src + '" ] && ! grep -q "' + dst + '" /proc/mounts; then ' +
      'mount -n --bind -o ro "' + src + '" "' + dst + '"; fi'
    );
  });
  return lines.join("\n");
}

/** Command that best-effort unmounts everything (used by disable). */
function cmdUnmountAll() {
  var lines = ["set +e"];
  GST_LIBS.forEach(function (lib) {
    var dst = GST_TARGET + "/" + lib;
    lines.push('grep -q "' + dst + '" /proc/mounts && umount "' + dst + '" 2>/dev/null');
  });
  lines.push('grep -q "' + GSTCOOL + '" /proc/mounts && umount "' + GSTCOOL + '" 2>/dev/null');
  lines.push('rm -f "' + GSTCOOL_TMP + '"');
  return lines.join("\n");
}

/* Read persisted config (returns defaults if absent). */
function readConfig() {
  return rootExec('cat "' + CONFIG_JSON + '" 2>/dev/null || true').then(function (r) {
    var cfg = null;
    try { cfg = JSON.parse(r.stdout); } catch (e) { cfg = null; }
    if (!cfg) return { downmix: Object.assign({}, DOWNMIX_DEFAULTS), dvDisabled: false };
    return {
      downmix: validateDownmix(cfg.downmix || {}),
      dvDisabled: !!cfg.dvDisabled
    };
  });
}

/* ======================================================================
 * Luna methods
 * ====================================================================== */

/* enable: stage payload -> persist config -> write init script ->
 *         apply mounts + gstcool now (no reboot). */
service.register("enable", function (message) {
  readConfig().then(function (cfg) {
    var downmix = cfg.downmix;
    var script = buildInitScript(downmix, cfg.dvDisabled);
    var steps = [
      cmdStagePayload(),
      cmdWriteConfig(downmix, cfg.dvDisabled),
      cmdWriteInitScript(script),
      cmdApplyMounts(),
      cmdApplyGstcool(downmix)
    ].join("\n\n");
    return rootExec(steps);
  }).then(function (r) {
    message.respond({ returnValue: true, applied: true, stdout: r.stdout, stderr: r.stderr });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* disable: remove the init stub + best-effort unmount now. Original LG
 * files are never touched, so this is fully reversible. */
service.register("disable", function (message) {
  var steps = [
    'rm -f "' + INIT_SCRIPT + '"',
    cmdUnmountAll()
  ].join("\n\n");
  rootExec(steps).then(function (r) {
    message.respond({ returnValue: true, removed: true, stdout: r.stdout, stderr: r.stderr });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* status: gather everything the UI shows in one root round-trip. */
service.register("status", function (message) {
  var probe = [
    'echo "MODEL=$(nyx-cmd DeviceInfo query product_id 2>/dev/null)"',
    'echo "WEBOS=$(nyx-cmd OSInfo query webos_release 2>/dev/null)"',
    'echo "GST=$(gst-inspect-1.0 --version 2>/dev/null | grep GStreamer | cut -d\' \' -f2)"',
    'echo "MOUNTS=$(grep -c gstreamer-1.0 /proc/mounts 2>/dev/null)"',
    'echo "RANK=$(grep -oE \'avdec_dca=[0-9]+\' ' + GSTCOOL + ' 2>/dev/null | head -1 | cut -d= -f2)"',
    'echo "INIT=$([ -e ' + INIT_SCRIPT + ' ] && echo 1 || echo 0)"',
    'echo "DVFLAG=$([ -f /tmp/dv_disable ] && echo 1 || echo 0)"',
    'echo "DVPERSIST=$([ -f ' + DV_PERSIST + ' ] && echo 1 || echo 0)"'
  ].join("\n");

  Promise.all([rootExec(probe), readConfig()]).then(function (results) {
    var out = results[0].stdout || "";
    var cfg = results[1];
    var kv = {};
    out.split("\n").forEach(function (line) {
      var i = line.indexOf("=");
      if (i > 0) kv[line.slice(0, i).trim()] = line.slice(i + 1).trim();
    });
    var mounts = parseInt(kv.MOUNTS, 10);
    var rank = kv.RANK ? parseInt(kv.RANK, 10) : null;
    message.respond({
      returnValue: true,
      model: kv.MODEL || "",
      webosVersion: kv.WEBOS || "",
      gstVersion: kv.GST || "",
      overridesMounted: (isFinite(mounts) && mounts > 0),
      mountCount: isFinite(mounts) ? mounts : 0,
      avdecDcaRank: (rank != null && isFinite(rank)) ? rank : null,
      initInstalled: kv.INIT === "1",
      dvDisabled: (kv.DVFLAG === "1") || (kv.DVPERSIST === "1"),
      downmix: cfg.downmix
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* setDownmix: validate -> persist -> re-apply gstcool + refresh init script. */
service.register("setDownmix", function (message) {
  var downmix;
  try {
    downmix = validateDownmix(message.payload || {});
  } catch (e) {
    message.respond({ returnValue: false, errorText: "invalid downmix: " + e.message });
    return;
  }
  readConfig().then(function (cfg) {
    var script = buildInitScript(downmix, cfg.dvDisabled);
    var steps = [
      cmdWriteConfig(downmix, cfg.dvDisabled),
      cmdWriteInitScript(script),
      cmdApplyGstcool(downmix)   // apply immediately if the init dir exists
    ].join("\n\n");
    return rootExec(steps).then(function (r) {
      message.respond({ returnValue: true, downmix: downmix, stdout: r.stdout, stderr: r.stderr });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* getDownmix: return persisted coefficients. */
service.register("getDownmix", function (message) {
  readConfig().then(function (cfg) {
    message.respond({ returnValue: true, downmix: cfg.downmix });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* setDvDisabled: create/remove /tmp/dv_disable now + persist for boot. */
service.register("setDvDisabled", function (message) {
  var disabled = !!(message.payload && message.payload.disabled);
  readConfig().then(function (cfg) {
    var nowCmd = disabled
      ? ': > /tmp/dv_disable'                 // create flag
      : 'rm -f /tmp/dv_disable';              // remove flag
    var persistCmd = disabled
      ? 'mkdir -p "' + STATE_DIR + '" && : > "' + DV_PERSIST + '"'
      : 'rm -f "' + DV_PERSIST + '"';
    // Keep config.json + init script in sync so a reboot honours the choice.
    var script = buildInitScript(cfg.downmix, disabled);
    var steps = [
      nowCmd,
      persistCmd,
      cmdWriteConfig(cfg.downmix, disabled),
      cmdWriteInitScript(script)
    ].join("\n\n");
    return rootExec(steps).then(function (r) {
      message.respond({ returnValue: true, dvDisabled: disabled, stdout: r.stdout, stderr: r.stderr });
    });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});

/* getConfig: full persisted config for the UI to hydrate from. */
service.register("getConfig", function (message) {
  readConfig().then(function (cfg) {
    message.respond({ returnValue: true, downmix: cfg.downmix, dvDisabled: cfg.dvDisabled });
  }).catch(function (e) {
    message.respond({ returnValue: false, errorText: e.errorText || e.message || String(e) });
  });
});
