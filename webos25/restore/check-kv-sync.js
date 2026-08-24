#!/usr/bin/env node
/* check-kv-sync.js — every KEY a generated shell emits must be consumed somewhere,
 * and nothing may consume a KEY no shell emits.
 *
 * Why this exists. A single fact crosses four hand-synced vocabularies: a shell
 * variable, an `echo "KEY=..."`, a JS read, and a UI field. Nothing generates one
 * from another, so they drift silently, and the drift is invisible to the other
 * check-*-sync scripts (they compare duplicated *scripts*, not this producer/consumer
 * seam). Three releases in one day came from exactly this:
 *
 *   2.7.12  C2_MOUNT_TS was set by the inspector but never echoed -> logs read "ts:?"
 *   2.7.13  the TS demuxer bound, but nothing could exercise it
 *   2.7.14  the self-test ran ts/m2ts and the handler discarded the results
 *
 * Each was "changed one place, not the others". This makes that a test failure.
 *
 * Consumption is deliberately loose: a key counts as consumed if its name appears
 * anywhere in the service or the UI outside the echo that emits it. That catches the
 * real bug (emitted, referenced nowhere) without fighting `kv.X` vs `p.X` vs the
 * dynamic `kv[c.key]` the webOS-25 self-test uses.
 *
 * KNOWN LIMITATION, stated rather than hidden: direction 1 cannot judge SHORT,
 * LOWERCASE keys. The self-test emits `mp4`, `ts` and `m2ts`, and those substrings
 * occur throughout service.js/app.js (`results`, `tRts`, ".ts" literals), so they
 * always look consumed. Verified: deleting ts/m2ts from the self-test handler's file
 * map still passes here. Direction 2 is the one that caught the 2.7.12 bug, and it is
 * exact because it only considers UPPER_SNAKE probe keys. If you add a short
 * lowercase key, this check will NOT protect it -- add a real assertion in
 * profile-compat.test.js instead.
 */
"use strict";
var fs = require("fs"), vm = require("vm"), path = require("path");

var ROOT = path.resolve(__dirname, "..");
var SERVICE = path.join(ROOT, "app/service/service.js");
var UI = path.join(ROOT, "app/js/app.js");

/* Keys that are deliberately write-only. Keep this list SHORT and justified; an
 * entry here is a promise that a human decided the key is for eyeballs only. */
var LOG_ONLY = {
  ELF_EFLAGS: "reported in the verdict block for a human to read",
  UNAME_M: "diagnostic only; FLOAT_ABI/LOADER are what gate",
  MKV_HAS_A_DTS_STRING: "recorded for firmware analysis, not branched on",
  MKV_HAS_XDTS_CAPS_STRING: "recorded for firmware analysis, not branched on",
  GST_MAJMIN: "human-facing field mirroring docs/detect-target.sh:109; GST_MM is the gated one"
};

function loadService() {
  var src = fs.readFileSync(SERVICE, "utf8") +
    "\nmodule.exports={DETECT_PROBE:DETECT_PROBE,c2StatusProbe:c2StatusProbe," +
    "c2SelfTest:c2SelfTest,w25SelfTest:w25SelfTest,w25StatusProbe:w25StatusProbe," +
    "c2Enable:c2Enable,c2Disable:c2Disable,c2InitScriptBody:c2InitScriptBody," +
    "w25Enable:w25Enable,w25Disable:w25Disable,w25Uninstall:w25Uninstall," +
    "cxEnable:cxEnable,cxDisable:cxDisable,cxUninstall:cxUninstall};";
  var FakeService = function () { this.register = function () {}; };
  var ctx = {
    require: function (n) { return n === "webos-service" ? FakeService : require(n); },
    module: {exports: {}}, exports: {}, Buffer: Buffer, console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout, Promise: Promise
  };
  vm.runInNewContext(src, ctx, {filename: SERVICE});
  return ctx.module.exports;
}

var svc = loadService();
/* Every generated script, not just the probes: the enable/disable/init generators
 * emit VERDICT/OK/STOOD_DOWN/REG_REVERTED and the handlers read them. */
var scripts = {
  DETECT_PROBE:      svc.DETECT_PROBE,
  c2StatusProbe:     svc.c2StatusProbe(),
  c2SelfTest:        svc.c2SelfTest(),
  c2Enable:          svc.c2Enable(true),
  c2Disable:         svc.c2Disable(false),
  c2DisableRemove:   svc.c2Disable(true),
  c2InitScriptBody:  svc.c2InitScriptBody(),
  w25StatusProbe:    svc.w25StatusProbe(),
  w25SelfTest:       svc.w25SelfTest(),
  w25Enable:         svc.w25Enable(true),
  w25Disable:        svc.w25Disable(),
  w25Uninstall:      svc.w25Uninstall(),
  cxEnable:          svc.cxEnable(),
  cxDisable:         svc.cxDisable(),
  cxUninstall:       svc.cxUninstall()
};

var serviceSrc = fs.readFileSync(SERVICE, "utf8");
var uiSrc = fs.readFileSync(UI, "utf8");

function emittedKeys(text) {
  var out = {};
  var re = /echo\s+"([A-Za-z_][A-Za-z0-9_]*)=/g, m;
  while ((m = re.exec(String(text)))) out[m[1]] = true;
  return Object.keys(out).sort();
}

/* Keys emitted through a loop variable, e.g. the C2 self-test's
 * `for c in ts m2ts; do ... echo "$c=PASS:$SZ"` -- the real shape of the 2.7.14 bug,
 * where ts= and m2ts= were produced and the handler discarded them. Expand the loop's
 * literal word list so those count as emitted keys. */
function loopEmittedKeys(text) {
  var out = {};
  var t = String(text);
  var loops = /for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+([A-Za-z0-9_ .-]+?)\s*;\s*do/g, lm;
  while ((lm = loops.exec(t))) {
    var v = lm[1], words = lm[2].trim().split(/\s+/);
    if (!new RegExp('echo\\s+"\\$(?:\\{)?' + v + '(?:\\})?=').test(t)) continue;
    words.forEach(function (w) { if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(w)) out[w] = true; });
  }
  return Object.keys(out);
}

/* Some keys are emitted with a computed name, e.g. DETECT_PROBE's
 * `echo "C2_FP_${key}=$v"` inside a `for k in hardware_id product_id ...` loop.
 * A literal-echo scan cannot see those, so collect the prefixes and treat any read
 * key starting with one as produced. Without this the C2_FP_* family reads as 18
 * phantom failures. */
function emittedPrefixes(text) {
  var out = {};
  var re = /echo\s+"([A-Za-z_][A-Za-z0-9_]*_)\$\{/g, m;
  while ((m = re.exec(String(text)))) out[m[1]] = true;
  return Object.keys(out);
}

/* Occurrences of `KEY` that are NOT the `echo "KEY=` that emits it. */
function consumedSomewhere(key) {
  var word = new RegExp("\\b" + key + "\\b", "g");
  var emit = new RegExp('echo\\s+"' + key + "=", "g");
  var total = (serviceSrc.match(word) || []).length + (uiSrc.match(word) || []).length;
  var emits = (serviceSrc.match(emit) || []).length;
  return total - emits > 0;
}

var failures = [];
var checked = 0;
Object.keys(scripts).forEach(function (name) {
  emittedKeys(scripts[name]).concat(loopEmittedKeys(scripts[name])).forEach(function (key) {
    checked++;
    if (LOG_ONLY[key]) return;
    if (!consumedSomewhere(key)) {
      failures.push(name + " emits " + key + ", but nothing reads it");
    }
  });
});

/* Direction 2: the detect/status seam specifically.
 *
 * Every key read inside compatVerdict() and logDiagnostic() comes from DETECT_PROBE
 * output, so DETECT_PROBE must emit it. This is the direction the 2.7.12 bug took:
 * logDiagnostic read C2_MOUNT_TS while no echo produced it, so the field silently
 * logged "?" and the drift was invisible.
 *
 * Deliberately scoped to those two functions. A blanket "every kv.UPPER in the file"
 * rule is wrong here: kv also carries the persisted baseline (C2_FP_*, read via
 * c2_fp from the baseline file), gain-config output, and A/B meter output, none of
 * which DETECT_PROBE emits.
 */
function bodyOf(name) {
  var i = serviceSrc.indexOf("function " + name + "(");
  if (i < 0) return "";
  var depth = 0, started = false;
  for (var j = i; j < serviceSrc.length; j++) {
    var ch = serviceSrc[j];
    if (ch === "{") { depth++; started = true; }
    else if (ch === "}") { depth--; if (started && depth === 0) return serviceSrc.slice(i, j + 1); }
  }
  return serviceSrc.slice(i);
}
var probeKeys = {};
emittedKeys(scripts.DETECT_PROBE).forEach(function (k) { probeKeys[k] = true; });
var probePrefixes = emittedPrefixes(scripts.DETECT_PROBE);
function probeEmits(key) {
  return !!probeKeys[key] || probePrefixes.some(function (pre) { return key.indexOf(pre) === 0; });
}
["compatVerdict", "logDiagnostic"].forEach(function (fn) {
  var body = bodyOf(fn);
  if (!body) { failures.push("check-kv-sync could not find " + fn + "()"); return; }
  var re = /\bkv\.([A-Z][A-Z0-9_]{2,})\b/g, m, seen = {};
  while ((m = re.exec(body))) seen[m[1]] = true;
  Object.keys(seen).sort().forEach(function (key) {
    checked++;
    if (!probeEmits(key)) {
      failures.push(fn + "() reads " + key + ", but DETECT_PROBE does not emit it");
    }
  });
});

/* Stale allowlist entries are themselves a defect: they hide a key that no longer exists. */
Object.keys(LOG_ONLY).forEach(function (key) {
  var found = Object.keys(scripts).some(function (n) {
    return emittedKeys(scripts[n]).indexOf(key) >= 0;
  });
  if (!found) failures.push("LOG_ONLY lists " + key + ", but no script emits it any more");
});

console.log("checked " + checked + " emitted keys across " +
  Object.keys(scripts).length + " generated scripts");
if (failures.length) {
  failures.forEach(function (f) { console.error("FAIL: " + f); });
  console.error("FAILED: " + failures.length + " producer/consumer mismatch(es)");
  process.exit(1);
}
console.log("ALL CHECKS PASSED (every emitted key is consumed)");
