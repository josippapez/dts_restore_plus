"use strict";

/* Executable host tests for the C2 transaction engine.  The service is loaded in
 * a VM (webos-service is a TV-only module), while every transaction below runs
 * as a real /bin/sh process against a temporary author-table override. */
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var vm = require("node:vm");
var spawnSync = require("node:child_process").spawnSync;

var ROOT = path.resolve(__dirname, "..", "..", "..", "..");
var SERVICE = path.join(ROOT, "webos25/app/service/service.js");

function loadService(mocks) {
  var source = fs.readFileSync(SERVICE, "utf8") + "\nmodule.exports = {" +
    "compatVerdict: compatVerdict, isKnownProfile: isKnownProfile," +
    "c2OwnerRoute: c2OwnerRoute," +
    "c2Config: c2Config, c2InitScriptBody: c2InitScriptBody," +
    "c2Enable: c2Enable, c2Disable: c2Disable, c2StatusProbe: c2StatusProbe," +
    "c2StatusBindsComplete: c2StatusBindsComplete, c2SelfTest: c2SelfTest," +
    "DETECT_PROBE: DETECT_PROBE, w25EdidTarget: w25EdidTarget, w25InitScriptBody: w25InitScriptBody," +
    "w25PtReceiver: w25PtReceiver, w25PassthroughSteps: w25PassthroughSteps, w25Enable: w25Enable," +
    "w25Disable: w25Disable," +
    "constants: {C2: PROFILE_C2," +
    "sets: C2_EXPECTED_SETS, matchSet: c2MatchSet," +
    "libav: C2_EXPECTED_SETS[0].libav, iso: C2_EXPECTED_SETS[0].iso, mkv: C2_EXPECTED_SETS[0].mkv}};";
  var handlers = {};
  var FakeService = function () { this.register = function (name, handler) { handlers[name] = handler; }; };
  var context = {
    require: function (name) { return name === "webos-service" ? FakeService : require(name); },
    module: {exports: {}}, exports: {}, Buffer: Buffer, console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout
  };
  vm.runInNewContext(source, context, {filename: SERVICE});
  Object.keys(mocks || {}).forEach(function (name) { context[name] = mocks[name]; });
  context.module.exports.handlers = handlers;
  return context.module.exports;
}

var service = loadService();
var C = service.constants;

function sh(script, env) {
  var e = Object.assign({}, process.env, env || {});
  return spawnSync("sh", ["-c", script], {encoding: "utf8", env: e, timeout: 30000});
}
function write(file, text, mode) {
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, text || "");
  if (mode) fs.chmodSync(file, mode);
}
function exe(file, text) { write(file, "#!/bin/sh\n" + text + "\n", 0o755); }

/* A fixture contains only temporary paths.  Fake commands intentionally fail
 * through environment flags, so each failure is deterministic and observable. */
function fixture() {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-c2-"));
  var bin = path.join(dir, "bin");
  var state = path.join(dir, "state"), gst = path.join(state, "gst");
  var target = path.join(dir, "target"), payload = path.join(dir, "payload");
  var mountinfo = path.join(dir, "mountinfo"), registry = path.join(dir, "registry.bin");
  var payloadts = path.join(dir, "payload-ts");
  fs.mkdirSync(payloadts, {recursive: true});
  fs.mkdirSync(bin, {recursive: true});
  [target, payload, path.join(dir, "etc")].forEach(function (p) { fs.mkdirSync(p, {recursive: true}); });
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", "libgstisomp4_1_8.so"].forEach(function (f) {
    write(path.join(payload, f), "payload-" + f);
    write(path.join(target, f), "stock-" + f);
  });
  write(path.join(target, "libgstcoreelements.so"), "\0".repeat(36) + "\0\x02\0\0");
  write(path.join(dir, "etc/gstcool.conf"), "avdec_dca=0\n");
  write(registry, "stock-registry");
  write(mountinfo, "1 0 0:1 / / rw - fake root rw\n");
  exe(path.join(bin, "nyx-cmd"), 'case "$*" in *hardware_id) echo HE_DTV_W22O_AFABATAA;; *product_id) echo OLED65C2PUA;; *board_type) echo o22;; *webos_manufacturing_version) echo 04.40.93.01;; *webos_release) echo 7.4.0;; esac');
  exe(path.join(bin, "sha256sum"), '[ "${FAIL_HASH:-}" = 1 ] && exit 1; case "$1" in *libgstlibav.so) h="' + C.libav + '";; *libgstisomp4.so) h="' + C.iso + '";; *libgstmatroska.so) h="' + C.mkv + '";; *gstcool.conf) h="' + "a".repeat(64) + '";; *) exec /usr/bin/shasum -a 256 "$1";; esac; echo "$h  $1"');
  exe(path.join(bin, "gst-inspect-1.0"), '[ "$1" = --version ] && { echo "GStreamer 1.18.2"; exit 0; }; [ "${FAIL_INSPECT:-}" = 1 ] && exit 1; if [ -n "${GST_REGISTRY_1_0:-}" ] && [ "$#" -eq 0 ]; then printf registry > "$GST_REGISTRY_1_0" || exit 1; fi; exit 0');
  exe(path.join(bin, "gst-launch-1.0"), '[ "${FAIL_LAUNCH:-}" = 1 ] && exit 1; out=; for a in "$@"; do case "$a" in location=*) out=${a#location=};; esac; done; [ -n "$out" ] || exit 1; dd if=/dev/zero of="$out" bs=100001 count=1 >/dev/null 2>&1');
  exe(path.join(bin, "timeout"), 'shift; exec "$@"');
  exe(path.join(bin, "stat"), '[ "$1" = -c%s ] && exec /usr/bin/stat -f%z "$2"; exec /usr/bin/stat "$@"');
  // MISS_DEP reproduces the real shape of an unresolved dependency: the loader
  // SUCCEEDS and reports the missing library in its trace output. FAIL_TRACE is the
  // different case where the loader itself cannot run.
  exe(path.join(bin, "loader"), '[ "${FAIL_TRACE:-}" = 1 ] && { echo "not found"; exit 1; }; ' +
    '[ "${MISS_DEP:-}" = 1 ] && { echo "\tlibgstsubtitle-1.0.so.0 => not found"; exit 0; }; ' +
    // TS_MISS_DEP fails the trace for the TS demuxer ONLY: that case must degrade
    // to MP4/MKV, never fail the whole enable.
    'case "${1:-}" in *libgstmpegtsdemux.so) [ "${TS_MISS_DEP:-}" = 1 ] && { echo "\tlibgstmpegts-1.0.so.0 => not found"; exit 0; };; esac; exit 0');
  [["cp", "/bin/cp"], ["rm", "/bin/rm"], ["rmdir", "/bin/rmdir"], ["mkdir", "/bin/mkdir"], ["mv", "/bin/mv"], ["chmod", "/bin/chmod"], ["ln", "/bin/ln"], ["sed", "/usr/bin/sed"]].forEach(function (entry) {
    exe(path.join(bin, entry[0]), '[ "${FAIL_' + entry[0].toUpperCase() + ':-}" = 1 ] && exit 1; [ -n "${FAIL_' + entry[0].toUpperCase() + '_PATH:-}" ] && { for a in "$@"; do [ "$a" = "$FAIL_' + entry[0].toUpperCase() + '_PATH" ] && exit 1; done; }; exec ' + entry[1] + ' "$@"');
  });
  exe(path.join(bin, "mount"), 'if [ "${FAIL_MOUNT:-}" = 1 ]; then case "${INJECT_STATE_ENTRY:-}" in file) printf foreign > "' + state + '/foreign-entry";; hidden) printf foreign > "' + state + '/.foreign-hidden";; dir) /bin/mkdir "' + state + '/foreign-dir";; symlink) /bin/ln -s "' + dir + '" "' + state + '/foreign-link";; esac; exit 1; fi; target=""; source=""; for a in "$@"; do source="$target"; target="$a"; done; printf "2 1 0:1 %s %s rw - fake fake rw\n" "$source" "$target" >> "' + mountinfo + '"');
  exe(path.join(bin, "umount"), '[ "${FAIL_UMOUNT:-}" = 1 ] && exit 1; target=""; for a in "$@"; do target="$a"; done; tmp="' + mountinfo + '.tmp"; grep -v " $target " "' + mountinfo + '" > "$tmp"; mv "$tmp" "' + mountinfo + '"');
  return {dir: dir, bin: bin, state: state, gst: gst, target: target, payload: payload, payloadts: payloadts,
    mountinfo: mountinfo, registry: registry, hook: path.join(dir, "hook"), legacy: path.join(dir, "legacy"),
    init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"),
    registrySource: path.join(state, "registry.bin"), gstcool: path.join(dir, "etc/gstcool.conf"),
    core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"),
    command: function (name) { return path.join(bin, name); },
    overrides: function () { return {state: state, gst: gst, owner: path.join(state, "owner"), baseline: path.join(state, "baseline"), recovery: path.join(state, "recovery"), init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"), registrySource: path.join(state, "registry.bin"), hook: path.join(dir, "hook"), legacyHook: path.join(dir, "legacy"), payload: payload, payloadts: payloadts, mountinfo: mountinfo, gstTarget: target, gstcool: path.join(dir, "etc/gstcool.conf"), core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"), inspect: path.join(bin, "gst-inspect-1.0"), mount: path.join(bin, "mount"), umount: path.join(bin, "umount"), cp: path.join(bin, "cp"), rm: path.join(bin, "rm"), rmdir: path.join(bin, "rmdir"), mkdir: path.join(bin, "mkdir"), mv: path.join(bin, "mv"), chmod: path.join(bin, "chmod"), ln: path.join(bin, "ln"), readlink: "/usr/bin/readlink", sed: path.join(bin, "sed")}; }
  };
}
function run(f, script, extra) { return sh(script, Object.assign({PATH: f.bin + ":" + process.env.PATH, GST_REGISTRY_1_0: f.registry}, extra || {})); }
function ok(result, message) { assert.equal(result.status, 0, message + "\n" + result.stderr + "\n" + result.stdout); }
function test(name, fn) {
  function passed() { console.log("ok - " + name); }
  function failed(error) { console.error("not ok - " + name + "\n" + error.stack); process.exitCode = 1; }
  try {
    var result = fn();
    if (result && typeof result.then === "function") return result.then(passed).catch(failed);
    passed();
  } catch (error) { failed(error); }
}

test("accepted C2 artifact sets match per-set and reject chimeras", function () {
  var sets = C.sets, match = C.matchSet;
  assert.ok(sets.length >= 2, "more than one analyzed C2 firmware should be accepted");
  sets.forEach(function (t, i) {
    assert.ok(match(t.libav, t.iso, t.mkv), "set " + (i + 1) + " must match itself exactly");
    assert.ok(t.label && t.label.length, "set " + (i + 1) + " must carry a human label");
    [t.libav, t.iso, t.mkv].forEach(function (h) {
      assert.match(h, /^[0-9a-f]{64}$/, "hashes must be lowercase sha256");
    });
  });
  // The whole point of matching per SET: a triple assembled from two different
  // firmwares must never pass, or the gate stops being an exact-match gate.
  assert.equal(match(sets[0].libav, sets[1].iso, sets[0].mkv), null, "chimera 1+2 must not match");
  assert.equal(match(sets[1].libav, sets[0].iso, sets[1].mkv), null, "chimera 2+1 must not match");
  assert.equal(match("x", "y", "z"), null, "junk must not match");
  assert.equal(match(undefined, undefined, undefined), null, "absent hashes must not match");
  // Every accepted set must be distinct, or a duplicate row hides a real mismatch.
  var seen = {};
  sets.forEach(function (t) {
    var k = t.libav + t.iso + t.mkv;
    assert.equal(seen[k], undefined, "duplicate artifact set: " + t.label);
    seen[k] = 1;
  });
});

test("transaction helpers execute with safe paths and generated shell is valid", function () {
  var f = fixture(), o = f.overrides();
  [service.c2InitScriptBody(o), service.c2Enable(true, o), service.c2Disable(true, o), service.c2StatusProbe(o), service.c2SelfTest(o)].forEach(function (script) { ok(spawnSync("sh", ["-n"], {input: script, encoding: "utf8"}), "generated C2 shell syntax"); });
  var r = run(f, service.c2Enable(true, o));
  ok(r, "first forced enable");
  assert.match(r.stdout + r.stderr, /OK/);
  assert.ok(fs.existsSync(path.join(f.state, "owner")), "successful enable must persist dedicated owner");
  assert.ok(fs.existsSync(path.join(f.state, "baseline")), "successful enable must persist baseline");
});

test("all four payload copies and loader tracing are required", function () {
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", "libgstisomp4_1_8.so"].forEach(function (missing) {
    var f = fixture(), o = f.overrides(); fs.unlinkSync(path.join(f.payload, missing));
    var r = run(f, service.c2Enable(true, o)); ok(r, "missing " + missing);
    assert.match(r.stdout, /payload|cleanup/); assert.equal(fs.existsSync(path.join(f.state, "owner")), false, "payload failure must not create ownership");
  });
  var f = fixture(), o = f.overrides(), r;
  f = fixture(); o = f.overrides(); r = run(f, service.c2Enable(true, o), {FAIL_TRACE: "1"});
  ok(r, "loader trace refusal");
  assert.match(r.stdout, /payload|cleanup/);

  // An unresolved dependency must name the file AND the library. Reporting only
  // "one of four payload copies or loader traces failed" sent issue #1 in circles:
  // the trace files that hold the answer are deleted by the rollback right after.
  f = fixture(); o = f.overrides(); r = run(f, service.c2Enable(true, o), {MISS_DEP: "1"});
  ok(r, "missing dependency refusal");
  assert.match(r.stdout, /payload/);
  assert.match(r.stdout, /libgstlibav\.so is missing dependencies on this TV/);
  assert.match(r.stdout, /libgstsubtitle-1\.0\.so\.0/);
  assert.equal(fs.existsSync(path.join(f.state, "owner")), false,
    "a missing dependency must not leave an owner marker behind");
});

test("missing baseline, recovery, partial, stacked, and foreign mounts refuse before mutation", function () {
  var f = fixture(), o = f.overrides();
  write(o.owner, "owned");
  var r = run(f, service.c2InitScriptBody(o)); ok(r, "missing baseline refusal"); assert.match(r.stdout, /owner exists without complete baseline/);
  f = fixture(); o = f.overrides(); write(o.owner, "owned"); write(o.recovery, "recover"); r = run(f, service.c2InitScriptBody(o)); ok(r, "recovery mode"); assert.match(r.stdout, /owner exists without complete baseline/);
  f = fixture(); o = f.overrides(); write(o.mountinfo, "1 0 0:1 / / rw - fake root rw\n2 1 0:1 " + f.gst + "/libgstlibav.so " + f.target + "/libgstlibav.so rw - fake fake rw\n"); r = run(f, service.c2Enable(true, o)); ok(r, "partial mount refusal"); assert.match(r.stdout, /foreign|managed target/);
  f = fixture(); o = f.overrides(); write(o.mountinfo, "1 0 0:1 / / rw - fake root rw\n2 1 0:1 /foreign " + f.target + "/libgstlibav.so rw - fake fake rw\n3 1 0:1 " + f.gst + "/libgstlibav.so " + f.target + "/libgstlibav.so rw - fake fake rw\n"); r = run(f, service.c2Enable(true, o)); ok(r, "stacked/foreign mount refusal"); assert.match(r.stdout, /foreign|ambiguous/);
});

test("broken hooks and changed-profile ownership are refusal contracts", function () {
  var f = fixture(), o = f.overrides();
  var r = run(f, service.c2Enable(true, o)); ok(r, "prepare owned hook"); assert.match(r.stdout, /OK/);
  write(o.hook, "foreign hook");
  r = run(f, service.c2Disable(false, o)); ok(r, "foreign hook refusal"); assert.match(r.stdout, /C2 hook is foreign/);
  var kv = {C2_OWNED: "1", C2_BASELINE_VALID: "1", C2_LIBAV_SHA256: C.libav, C2_ISOMP4_SHA256: C.iso, C2_MATROSKA_SHA256: C.mkv};
  assert.equal(service.compatVerdict("webos23-w23o-diagnostic", kv).verdict, "drift");
  assert.equal(service.compatVerdict("webos23-w23o-diagnostic", kv).canForce, false);
  assert.equal(service.c2OwnerRoute("enable", "webos23-w23o-diagnostic", {verdict: "drift"}), "refuse");
  assert.equal(service.c2OwnerRoute("enable", C.C2, {verdict: "forced"}), "enable");
  assert.equal(service.c2OwnerRoute("disable", "unknown", null), "disable");
});

test("apply, detach, hook, and cleanup failures retain a truthful recovery state", function () {
  var failures = [
    ["mount", {FAIL_MOUNT: "1"}, /recovery|bind failed/],
    ["inspect", {FAIL_INSPECT: "1"}, /recovery|registry generation|proof/]
  ];
  failures.forEach(function (x) { var f = fixture(), o = f.overrides(), r = run(f, service.c2Enable(true, o), x[1]); ok(r, x[0] + " failure"); assert.match(r.stdout, x[2], x[0] + " failure must be reported"); });
  var detach = fixture(), detachOverrides = detach.overrides();
  var enabled = run(detach, service.c2Enable(true, detachOverrides)); ok(enabled, "prepare owned mounts for detach failure"); assert.match(enabled.stdout, /OK/);
  var detached = run(detach, service.c2Disable(false, detachOverrides), {FAIL_UMOUNT: "1"}); ok(detached, "umount failure"); assert.match(detached.stdout, /recovery|detach/, "umount failure must be reported"); assert.ok(fs.existsSync(detachOverrides.recovery), "incomplete detach retains recovery marker");
  var cleanup = fixture(), cleanupOverrides = cleanup.overrides(); enabled = run(cleanup, service.c2Enable(true, cleanupOverrides)); ok(enabled, "prepare cleanup failure");
  var cleanupResult = run(cleanup, service.c2Disable(true, cleanupOverrides), {FAIL_RM: "1"}); ok(cleanupResult, "cleanup failure"); assert.match(cleanupResult.stdout, /recovery|cleanup|remove exact hook/); assert.ok(fs.existsSync(cleanupOverrides.owner), "cleanup failure retains ownership");
});

test("copy, write-commit, hook, config, and cleanup command faults execute fail-closed", function () {
  [["FAIL_CP", /payload|cleanup/], ["FAIL_MV", /baseline|cleanup|state/], ["FAIL_CHMOD", /init|hook|rollback|recovery/], ["FAIL_SED", /config|recovery/]].forEach(function (scenario) {
    var f = fixture(), o = f.overrides(), env = {}; env[scenario[0]] = "1";
    var r = run(f, service.c2Enable(true, o), env); ok(r, scenario[0]); assert.match(r.stdout, scenario[1], scenario[0] + " must fail closed");
  });
  [["baseline", "baseline"], ["owner", "owner"], ["env", "registry config"], ["init", "init script"], ["recovery", "recovery marker"], ["configSource", "config"], ["registrySource", "registry"]].forEach(function (scenario) {
    var f = fixture(), o = f.overrides(), external = path.join(f.dir, "blocked-" + scenario[0]); o[scenario[0]] = external; var blockedPath = scenario[0] === "baseline" || scenario[0] === "init" ? external + ".tmp" : external; fs.mkdirSync(blockedPath, {recursive: true});
    var r = run(f, service.c2Enable(true, o)); ok(r, scenario[0] + " write failure"); assert.match(r.stdout, new RegExp(scenario[1] + "|rollback|recovery|cleanup"), scenario[0] + " write must fail closed");
  });
  var f = fixture(), o = f.overrides();
  var r = run(f, service.c2Enable(true, o), {FAIL_MV_PATH: o.hook}); ok(r, "hook install failure");
  assert.match(r.stdout, /hook|rollback|recovery/);
});

test("disabled lifecycle, recovery boot, and foreign status remain coherent", function () {
  var f = fixture(), o = f.overrides(), r = run(f, service.c2Enable(true, o)); ok(r, "enable before disable"); assert.match(r.stdout, /OK/);
  r = run(f, service.c2Disable(false, o)); ok(r, "disable"); assert.match(r.stdout, /OK/); assert.equal(fs.existsSync(o.hook), false); assert.equal(fs.existsSync(o.owner), true);
  r = run(f, service.c2Enable(false, o)); ok(r, "re-enable disabled owner"); assert.match(r.stdout, /OK/); assert.ok(fs.readFileSync(o.hook, "utf8").indexOf("exec") >= 0);
  r = run(f, service.c2Disable(false, o)); ok(r, "disable before uninstall"); assert.match(r.stdout, /OK/);
  r = run(f, service.c2Disable(true, o)); ok(r, "uninstall disabled owner"); assert.match(r.stdout, /OK/); assert.equal(fs.existsSync(f.state), false);

  f = fixture(); o = f.overrides(); r = run(f, service.c2Enable(true, o)); ok(r, "enable before recovery boot"); write(o.recovery, "recover");
  r = run(f, service.c2InitScriptBody(o)); ok(r, "recovery boot"); assert.match(r.stdout, /recovery mode detached owned mounts/); assert.equal(fs.existsSync(o.owner), true); assert.equal(fs.existsSync(o.recovery), true);

  f = fixture(); o = f.overrides(); write(o.mountinfo, "1 0 0:1 / / rw - fake root rw\n2 1 0:1 /foreign " + f.target + "/libgstlibav.so rw - fake fake rw\n");
  r = run(f, service.c2StatusProbe(o)); ok(r, "foreign status"); assert.match(r.stdout, /INSPECT=0/); assert.match(r.stdout, /MKV=none/); assert.match(r.stdout, /REGISTRY=none/);
});

test("registry, recovery-marker, namespace, hook-removal, and init-tamper failures retain safe state", function () {
  var f = fixture(), o = f.overrides();
  var missingRegistry = path.join(f.dir, "missing-registry");
  var r = run(f, service.c2Enable(true, o), {GST_REGISTRY_1_0: missingRegistry});
  ok(r, "invalid registry refusal");
  assert.match(r.stdout, /registry/);
  assert.equal(fs.existsSync(f.state), false, "registry validation must precede staging");

  f = fixture(); o = f.overrides();
  fs.symlinkSync(path.join(f.dir, "missing-state-target"), f.state);
  r = run(f, service.c2Enable(true, o)); ok(r, "dangling namespace refusal");
  assert.match(r.stdout, /unowned C2 state/);
  assert.equal(fs.lstatSync(f.state).isSymbolicLink(), true, "foreign dangling namespace must remain untouched");

  f = fixture(); o = f.overrides();
  fs.unlinkSync(path.join(f.payload, "libgstmatroska.so"));
  o.recovery = path.join(f.dir, "blocked-recovery"); fs.mkdirSync(o.recovery);
  r = run(f, service.c2Enable(true, o)); ok(r, "marker-before-cleanup refusal");
  assert.match(r.stdout, /recovery marker could not be written/);
  assert.equal(fs.existsSync(f.state), true, "cleanup must not run without a durable recovery marker");

  f = fixture(); o = f.overrides();
  r = run(f, service.c2Enable(true, o), {FAIL_MOUNT: "1", FAIL_RM_PATH: o.hook});
  ok(r, "rollback hook-removal failure");
  assert.match(r.stdout, /hook removal failed|recovery state was retained|exact hook and dedicated state were retained/);
  [o.owner, o.baseline, o.recovery, o.hook].forEach(function (file) {
    assert.equal(fs.existsSync(file), true, "rollback failure must retain " + file);
  });

  f = fixture(); o = f.overrides();
  r = run(f, service.c2Enable(true, o)); ok(r, "prepare init tamper"); assert.match(r.stdout, /OK/);
  var sentinel = path.join(f.dir, "tampered-init-ran");
  write(o.init, "#!/bin/sh\ntouch '" + sentinel + "'\n", 0o755);
  var hookRun = spawnSync("sh", [o.hook], {encoding: "utf8", env: Object.assign({}, process.env, {PATH: f.bin + ":" + process.env.PATH})});
  ok(hookRun, "authenticated hook guard");
  assert.equal(fs.existsSync(sentinel), false, "hook guard must not execute tampered init content");
  r = run(f, service.c2Disable(false, o)); ok(r, "tampered init teardown refusal");
  assert.match(r.stdout, /init content is foreign/);
  assert.equal(fs.existsSync(o.hook), true, "tampered init must not authorize hook removal");

  var guarded = fixture(), guardedOverrides = guarded.overrides();
  var enabled = run(guarded, service.c2Enable(true, guardedOverrides)); ok(enabled, "unexpected state setup");
  var before = fs.readFileSync(guarded.mountinfo, "utf8");
  [
    ["unexpected file", function () { var p = path.join(guarded.state, "foreign-entry"); write(p, "foreign"); return p; }],
    ["unexpected directory", function () { var p = path.join(guarded.state, "foreign-dir"); fs.mkdirSync(p); return p; }],
    ["unexpected symlink", function () { var p = path.join(guarded.state, "foreign-link"); fs.symlinkSync(guarded.dir, p); return p; }],
    ["unexpected hidden entry", function () { var p = path.join(guarded.state, ".foreign-hidden"); write(p, "foreign"); return p; }]
  ].forEach(function (caseSpec) {
    var foreign = caseSpec[1]();
    var refused = run(guarded, service.c2Disable(true, guardedOverrides));
    ok(refused, caseSpec[0] + " refusal");
    assert.match(refused.stdout, /unexpected entries or symlinks/, caseSpec[0] + " must be rejected by the allowlist");
    assert.equal(fs.readFileSync(guarded.mountinfo, "utf8"), before, caseSpec[0] + " must not detach mounts");
    assert.equal(fs.existsSync(guardedOverrides.recovery), false, caseSpec[0] + " must not write recovery before validation");
    assert.equal(fs.existsSync(guardedOverrides.hook), true, caseSpec[0] + " must retain the exact hook");
    assert.equal(fs.existsSync(guardedOverrides.owner), true, caseSpec[0] + " must retain ownership");
    if (fs.lstatSync(foreign).isDirectory() && !fs.lstatSync(foreign).isSymbolicLink()) fs.rmdirSync(foreign); else fs.unlinkSync(foreign);
  });

  ["file", "hidden", "dir", "symlink"].forEach(function (kind) {
    var rollback = fixture(), rollbackOverrides = rollback.overrides();
    var failed = run(rollback, service.c2Enable(true, rollbackOverrides), {FAIL_MOUNT: "1", INJECT_STATE_ENTRY: kind});
    ok(failed, "first-install unexpected " + kind);
    assert.match(failed.stdout, /unexpected C2 state was retained before detach/);
    [rollbackOverrides.owner, rollbackOverrides.baseline, rollbackOverrides.recovery, rollbackOverrides.init, rollbackOverrides.hook].forEach(function (file) {
      assert.equal(fs.existsSync(file), true, "first-install " + kind + " must retain coherent recovery artifact " + file);
    });
  });

  var cleanup = fixture(), cleanupOverrides = cleanup.overrides();
  enabled = run(cleanup, service.c2Enable(true, cleanupOverrides)); ok(enabled, "cleanup fault setup");
  [
    ["payload removal", function (o) { return {FAIL_RM_PATH: path.join(o.gst, "libgstlibav.so")}; }],
    ["gst rmdir", function (o) { return {FAIL_RMDIR_PATH: o.gst}; }],
    ["state rmdir", function (o) { return {FAIL_RMDIR_PATH: o.state}; }],
    ["final hook removal", function (o) { return {FAIL_RM_PATH: o.hook}; }]
  ].forEach(function (caseSpec) {
    var refused = run(cleanup, service.c2Disable(true, cleanupOverrides), caseSpec[1](cleanupOverrides));
    ok(refused, caseSpec[0] + " refusal"); assert.match(refused.stdout, /snapshot|cleanup|hook removal/);
    [cleanupOverrides.owner, cleanupOverrides.baseline, cleanupOverrides.recovery, cleanupOverrides.init, cleanupOverrides.hook].forEach(function (file) {
      assert.equal(fs.existsSync(file), true, caseSpec[0] + " must restore coherent recovery artifact " + file);
    });
  });
  assert.doesNotMatch(service.c2Disable(true, fixture().overrides()), /\brm\b[^\n]*-rf/, "C2 uninstall must not recursively delete state");
});

test("optional target absence, complete owned detach, dynamic tracing, and registry commands are exercised", function () {
  var complete = fixture(), completeOverrides = complete.overrides();
  var completeResult = run(complete, service.c2Enable(true, completeOverrides)); ok(completeResult, "prepare required optional bind"); assert.match(completeResult.stdout, /OK/);
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", "libgstisomp4_1_8.so"].forEach(function (name) {
    assert.equal(fs.lstatSync(path.join(complete.state, "trace." + name)).isFile(), true, "loader trace must be a regular known file: " + name);
  });
  write(complete.mountinfo, fs.readFileSync(complete.mountinfo, "utf8").split("\n").filter(function (line) {
    return line.indexOf(complete.target + "/libgstisomp4_1_8.so") < 0;
  }).join("\n"));
  var statusResult = run(complete, service.c2StatusProbe(completeOverrides)); ok(statusResult, "status with present optional target missing bind");
  var statusKv = {}; statusResult.stdout.trim().split("\n").forEach(function (line) { var at = line.indexOf("="); if (at > 0) statusKv[line.slice(0, at)] = line.slice(at + 1); });
  assert.equal(statusKv.ISO18_TARGET, "1"); assert.equal(statusKv.ISO18, "none");
  assert.equal(service.c2StatusBindsComplete(statusKv), false, "status must not call an incomplete optional bind active");
  completeResult = run(complete, service.c2SelfTest(completeOverrides)); ok(completeResult, "present optional target missing bind"); assert.match(completeResult.stdout, /REFUSED=inactive/);

  var f = fixture(), o = f.overrides(); fs.unlinkSync(path.join(f.target, "libgstisomp4_1_8.so"));
  var r = run(f, service.c2Enable(true, o)); ok(r, "optional target absent remains valid"); assert.match(r.stdout, /OK/);
  statusResult = run(f, service.c2StatusProbe(o)); ok(statusResult, "status with optional target absent");
  statusKv = {}; statusResult.stdout.trim().split("\n").forEach(function (line) { var at = line.indexOf("="); if (at > 0) statusKv[line.slice(0, at)] = line.slice(at + 1); });
  assert.equal(statusKv.ISO18_TARGET, "0"); assert.equal(statusKv.ISO18, "none");
  assert.equal(service.c2StatusBindsComplete(statusKv), true, "absent optional target remains complete");
  r = run(f, service.c2SelfTest(o)); ok(r, "owned registry self-test"); assert.match(r.stdout, /mp4=PASS/);
  /* Disable through the same generated helper; all owned records must be
   * detached before state removal. */
  r = run(f, service.c2Disable(true, o)); ok(r, "complete cleanup"); assert.match(r.stdout, /OK/); assert.equal(fs.existsSync(f.state), false, "complete cleanup removes only dedicated temporary state");
  r = run(f, service.c2SelfTest(o)); ok(r, "unowned self-test refusal"); assert.match(r.stdout, /REFUSED=state/);
  var init = service.c2InitScriptBody(o); assert.match(init, /LD_TRACE_LOADED_OBJECTS=1/); assert.match(init, /GST_REGISTRY_FORK=no/); assert.match(service.c2SelfTest(), /GST_REGISTRY_FORK=no/);
});

if (process.exitCode) process.exit(process.exitCode);

/* A registered dts_audiodec must not displace an exact-matched C2 profile.
 *
 * Regression for issue #1: an OLED55CS6LA matched all eleven C2 gates, but its
 * gstcool.conf carries dts_audiodec=290 + avdec_dca=0, so the behavioral override
 * relabelled it native-dts-gated and the opt-in button was never rendered. On the
 * C2 family that element registers over a 128 KB stub with no decoder internals,
 * so the override must yield to the exact gate -- while still firing for C3/C4,
 * where the decoder is real and the demuxer gate is the actual problem. */
test("dts_audiodec override yields to an exact C2 match but still fires elsewhere", function () {
  var probe = service.DETECT_PROBE;
  var start = probe.indexOf("# --- PROFILE SELECTION ---");
  var end = probe.indexOf('echo "PROFILE=$PROFILE"');
  assert.ok(start > 0 && end > start, "selection block not found in DETECT_PROBE");
  var block = probe.slice(start, end);

  function profileFor(v) {
    var pre = [
      "set -u",
      "GST_VERSION=" + v.gst,
      'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
      "HARDWARE_ID=" + v.hw,
      "PRODUCT_ID=" + v.pid,
      "BOARD_TYPE=" + (v.board || "O22_DVB"),
      "WEBOS_RELEASE=" + (v.webos || "9.2.2"),
      "WEBOS_MANUFACTURING_VERSION=" + (v.fw || "23.25.55"),
      "LOADER=" + (v.loader || "ld-linux.so.3"),
      "FLOAT_ABI=" + (v.abi || "soft"),
      "HAS_DTS_AUDIODEC=" + v.audiodec,
      "DCA_RANK=" + v.rank
    ].join("\n");
    var r = sh(pre + "\n" + block + '\nprintf "%s" "$PROFILE"');
    assert.strictEqual(r.status, 0, "selection block failed: " + r.stderr);
    // The block also echoes C2_GATE_FAIL, so take only the final printf.
    var lines = r.stdout.trim().split("\n");
    return lines[lines.length - 1].trim();
  }

  // The exact-matched C2/CS set from issue #1: keeps its own profile.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    audiodec: "yes", rank: "0"
  }), C.C2, "an exact C2 match must not be relabelled native-dts-gated");

  // C3: decoder is real, demuxer gate off -> the override is correct here.
  assert.strictEqual(profileFor({
    gst: "1.18.4", hw: "HE_DTV_W23O_AFAAATAA", pid: "OLED55C34LA",
    audiodec: "yes", rank: "0"
  }), "native-dts-gated");

  // C3 with the rank already raised -> plain native.
  assert.strictEqual(profileFor({
    gst: "1.18.4", hw: "HE_DTV_W23O_AFAAATAA", pid: "OLED55C34LA",
    audiodec: "yes", rank: "290"
  }), "native-dts");

  // A C2-family set that does NOT match the exact gate keeps the diagnostic
  // profile rather than silently claiming native DTS.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    fw: "99.99.99", audiodec: "no", rank: "0"
  }), "webos22-o22-c2-diagnostic");

  // ...and it must survive the override too: the diagnostic profile is the only one
  // that reports WHICH gate missed, so relabelling it hides that from the very TVs
  // that need it.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    fw: "99.99.99", audiodec: "yes", rank: "0"
  }), "webos22-o22-c2-diagnostic",
    "a near-miss C2 must keep its diagnostic profile, not become native-dts-gated");
});

/* Issue #3: a C1 on GStreamer 1.16.2 was refused as unknown, while upstream
 * dts_restore 2.0 (byte-identical to payload/cx) works there. */
test("a soft-float GStreamer 1.16 C1 selects the CX-payload profile", function () {
  var probe = service.DETECT_PROBE;
  var block = probe.slice(probe.indexOf("# --- PROFILE SELECTION ---"), probe.indexOf('echo "PROFILE=$PROFILE"'));
  function profileFor(loader, abi) {
    var pre = [
      "set -u", "GST_VERSION=1.16.2", 'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
      "HARDWARE_ID=HE_DTV_W21O_AFABATAA", "PRODUCT_ID=OLED55C11LB", "BOARD_TYPE=O20_DVB",
      "WEBOS_RELEASE=6.5.0", "WEBOS_MANUFACTURING_VERSION=03.51.16",
      "LOADER=" + loader, "FLOAT_ABI=" + abi, "HAS_DTS_AUDIODEC=no", "DCA_RANK=0"
    ].join("\n");
    var r = sh(pre + "\n" + block + '\nprintf "%s" "$PROFILE"');
    assert.strictEqual(r.status, 0, "selection block failed: " + r.stderr);
    var lines = r.stdout.trim().split("\n");
    return lines[lines.length - 1].trim();
  }
  assert.strictEqual(profileFor("ld-linux.so.3", "soft"), "webos6-armel-gst116");
  assert.strictEqual(profileFor("ld-linux-armhf.so.3", "hard"), "unknown-gst1.16-ld-linux-armhf.so.3");
  assert.ok(service.isKnownProfile("webos6-armel-gst116"));
  assert.strictEqual(service.compatVerdict("webos6-armel-gst116", {}), null);
});

/* Issue #7: a UR7500 reports edidType "ac3", and the opt-in only knew "TrueHD", so it
 * changed nothing. A hand edit there also left "ac3+dts+truehd" in configd's cache. */
test("the DTS-tracks opt-in maps ac3 to ac3+dts and replaces any cached value", function () {
  assert.strictEqual(service.w25EdidTarget("TrueHD", true), "TrueHD+dts");
  assert.strictEqual(service.w25EdidTarget("ac3", true), "ac3+dts");
  assert.strictEqual(service.w25EdidTarget("ac3+dts+truehd", true), "ac3+dts");
  assert.strictEqual(service.w25EdidTarget("ac3+dts", false), "ac3");
  assert.strictEqual(service.w25EdidTarget("TrueHD+dts", false), "TrueHD");
  assert.strictEqual(service.w25EdidTarget("ac3", false), "ac3");

  var body = service.w25InitScriptBody().split("\n");
  var fn = body.slice(body.indexOf("w25_appdts_target() {"));
  fn = fn.slice(0, fn.indexOf("}") + 1);
  function line(re) { var l = body.filter(function (x) { return re.test(x); }); assert.strictEqual(l.length, 1, String(re)); return l[0]; }
  var getStock = line(/^\s*EDID_STOCK=\$\(grep/);
  var getDts = line(/^\s*EDID_DTS=\$\(w25_appdts_target/);
  var patchCache = line(/sed -i .*\[\^\\"\]\*.*\$EDID_DTS.*CONFIGD_DB/).replace(/^\s*if /, "").replace(/ &&\s*$/, "");
  // The TV's busybox sed takes GNU-style -i; BSD sed needs an explicit empty suffix.
  if (process.platform === "darwin") patchCache = patchCache.replace("sed -i ", "sed -i '' ");
  function run(stockValue, cached) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-edid-"));
    var lls = path.join(dir, "factorydb.json"), db = path.join(dir, "configd_db.json");
    write(lls, '{"tv.model":{"edidType":"' + stockValue + '","oledCPC":1}}');
    write(db, '{"tv.model": {"edidType" : "' + cached + '", "oledCPC": 1}}');
    var r = sh(["set -u", "LOG=/dev/null", "LLS=" + lls, "CONFIGD_DB=" + db].concat(fn, [getStock, getDts, patchCache, 'printf "%s" "$EDID_DTS"']).join("\n"));
    assert.strictEqual(r.status, 0, r.stderr);
    return { target: r.stdout, db: fs.readFileSync(db, "utf8") };
  }
  var ur = run("ac3", "ac3+dts+truehd");
  assert.strictEqual(ur.target, "ac3+dts");
  assert.strictEqual(ur.db, '{"tv.model": {"edidType" : "ac3+dts", "oledCPC": 1}}');
  assert.strictEqual(run("TrueHD", "TrueHD").target, "TrueHD+dts");
  assert.strictEqual(run("eac3", "eac3").target, "");
});

function initFn(name) {
  var body = service.w25InitScriptBody().split("\n");
  var fn = body.slice(body.indexOf(name + "() {"));
  return fn.slice(0, fn.indexOf("}") + 1).join("\n");
}

/* Issues #5 and #10: on webOS 11.2 (43.21.77) the driver logs its completions at
 * lines 3491/3727 instead of 3410/3644, and the persisted log starts 0.5s after
 * audiooutputd. The old check missed a held port on the first and refused every
 * boot on the second. */
test("the audio-idle check reads webOS 11.2 logs", function () {
  var fn = initFn("w25_appdts_audio_idle");
  function line(ts, fnName, n, text) {
    return "2026-10-25T20:21:34.148484Z [" + ts + "] kern.notice kernel alsasndout NOT (03107:03382) " +
      fnName + "           " + n + ": " + text;
  }
  var connect = [line("3.279607580", "pmlog", 1, "boot"),
    line("18.6", "sndout_connect", 3305, "Start Connect output 0x1 input 0x1 port 1."),
    line("18.7", "sndout_connect", 3491, "output 0x1 input 0x1 port 1 .")];
  var disconnect = [line("40.1", "sndout_disconnect", 3569, "Start Disconnect output 0x1 input 0x1 port 1 ."),
    line("40.2", "sndout_disconnect", 3727, "output 0x1 input 0x1 port 1 .")];
  function idle(lines, aodUs) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-idle-"));
    var log = path.join(dir, "legacy-log");
    write(log, lines.join("\n") + "\n");
    var r = sh(["systemctl() { echo " + aodUs + "; }", fn.split("/var/log/legacy-log").join(log), "w25_appdts_audio_idle"].join("\n"));
    return r.status === 0;
  }
  assert.strictEqual(idle(connect.concat(disconnect), 2777195), true, "freed port, log 0.5s after audiooutputd");
  assert.strictEqual(idle(connect, 2777195), false, "held port, log 0.5s after audiooutputd");
  assert.strictEqual(idle(connect, 3000000), false, "held port, 11.2 line numbers");
  assert.strictEqual(idle(connect.concat(disconnect), 279607), false, "log starts 3s after audiooutputd");
});

/* Issue #10: webOS 11 refuses the service's Homebrew Channel calls ("Message not
 * processed") until org.webosbrew.hbchannel.service.group is in its client permissions. */
test("the boot hook adds the Homebrew Channel group to the service's permissions", function () {
  var fn = initFn("w25_hb_permission");
  if (process.platform === "darwin") fn = fn.replace("sed -i ", "sed -i '' ");
  function run(content) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-perm-"));
    var f = path.join(dir, "perm.json");
    write(f, content);
    var r = sh(["w25_log() { :; }", "HB_GROUP=org.webosbrew.hbchannel.service.group", "HB_PERM=" + f, fn, "w25_hb_permission"].join("\n"));
    assert.strictEqual(r.status, 0, r.stderr);
    return fs.readFileSync(f, "utf8");
  }
  var added = '{"io.github.josippapez.dtsenabler.service*":["public","org.webosbrew.hbchannel.service.group"]}';
  assert.strictEqual(run('{"io.github.josippapez.dtsenabler.service*":["public"]}'), added);
  assert.strictEqual(run(added), added);
  var other = '{"io.github.josippapez.dtsenabler.service*":["public"],"other":["x"]}';
  assert.strictEqual(run(other), other);
});

/* Issue #12: the passthrough toggle. dtsdec only sends the bitstream to
 * external_arc with Pass Through or Auto; on speakers it would be loud noise. */
test("the passthrough toggle only reports a receiver for external_arc", function () {
  assert.strictEqual(service.w25PtReceiver("external_arc passThrough"), true);
  assert.strictEqual(service.w25PtReceiver("external_arc auto"), true);
  assert.strictEqual(service.w25PtReceiver("external_arc pcm"), false);
  assert.strictEqual(service.w25PtReceiver("tv_external_speaker auto"), false);
  assert.strictEqual(service.w25PtReceiver(""), false);
  [true, false].forEach(function (on) {
    var script = service.w25PassthroughSteps(on);
    var r = spawnSync("sh", ["-n"], { input: script, encoding: "utf8" });
    assert.strictEqual(r.status, 0, "generated script is not valid sh: " + r.stderr);
    assert.ok(script.includes(on ? "W25_PT=start" : "W25_PT=stop"));
  });
  // The boot script must handle that mode before it gets anywhere near applying.
  var body = service.w25InitScriptBody();
  var mode = body.indexOf('if [ -n "${W25_PT:-}" ]; then');
  assert.ok(mode !== -1 && mode < body.indexOf("# --- G) self-heal"),
    "W25_PT mode must exit before the apply path");
});

test("DTS-HD shares the passthrough lifecycle and Disable restores eARC first", function () {
  [true, false, undefined].forEach(function (hd) {
    var script = service.w25PassthroughSteps(true, hd);
    var r = spawnSync("sh", ["-n"], { input: script, encoding: "utf8" });
    assert.strictEqual(r.status, 0, "generated script is not valid sh: " + r.stderr);
    var stop = script.indexOf("\nW25_PT=stop ");
    assert.ok(Buffer.byteLength(JSON.stringify({command: script})) < 110 * 1024,
      "the toggle must fit within the luna message limit");
    assert.ok(stop >= 0 && stop < script.indexOf(': > "$FLAG"'), "stop before changing the passthrough marker");
    assert.ok(stop < script.indexOf("W25_PT=start"), "stop before starting the watcher");
    if (typeof hd === "boolean") {
      var marker = hd ? ': > "/var/lib/webosbrew/dts25/passthrough-hd"'
        : 'rm -f "/var/lib/webosbrew/dts25/passthrough-hd"';
      assert.ok(script.indexOf(marker) > stop, "stop before changing the HD marker");
    } else {
      assert.ok(!script.includes('> "/var/lib/webosbrew/dts25/passthrough-hd"'));
    }
  });
  var body = service.w25InitScriptBody();
  assert.ok(!body.includes("W25_HD="));
  assert.ok(body.includes("\nw25_pt_stop || exit 1\nw25_pt_start\n"),
    "boot apply must stop passthrough before restarting it after Disable");
  assert.ok(initFn("w25_pt_watch").includes("w25_pt_recover &"));
  assert.ok(initFn("w25_pt_stop").includes('timeout 5 "$HD_HELPER" --recover'));
  assert.ok(!body.includes("hdroute.sh"));
  assert.ok(service.w25PassthroughSteps(true, true).includes("/lgroute-control ]"));
  assert.ok(!service.w25PassthroughSteps(true, true).includes("HD_RUNNING"));
  var off = service.w25PassthroughSteps(false);
  var offStop = off.indexOf("\nW25_PT=stop ");
  assert.ok(offStop >= 0 && offStop < off.indexOf('rm -f "$FLAG"'), "stop before clearing the passthrough marker");
  var disable = service.w25Disable();
  var stop = disable.indexOf("\nw25_pt_stop || exit 1\n");
  assert.ok(stop >= 0 && stop < disable.indexOf('rm -f "/var/lib/webosbrew/init.d/restore_dts25"'),
    "Disable must stop passthrough before removing the hook");
  assert.ok(stop < disable.lastIndexOf("\nw25_drop_binds\n"), "Disable must stop passthrough before dropping binds");
});

test("CLI install decodes the canonical stop script before unmounting", function () {
  var installer = path.join(ROOT, "webos25/restore/install.sh");
  var source = fs.readFileSync(installer, "utf8");
  var command = source.split("\n").find(function (line) { return line.includes("| W25_PT=stop sh || exit 1"); });
  assert.ok(command && source.indexOf(command) < source.indexOf('\nfor T in '), "stop must precede unmounting");
  var result = spawnSync("sh", ["-c", command.replace("| W25_PT=stop sh || exit 1", "| cat"), installer],
    {encoding: "utf8", timeout: 2000});
  assert.ifError(result.error);
  ok(result, "decode embedded stop script without executing it");
  assert.equal(result.stdout, service.w25InitScriptBody());
});

test("CLI uninstall rejects outdated init and stops before hook removal on failure", function () {
  var source = fs.readFileSync(path.join(ROOT, "webos25/restore/uninstall.sh"), "utf8");
  var start = source.indexOf('\nSTOP_SCRIPT=');
  var end = source.indexOf('\n# --- 1. Remove boot hook');
  assert.ok(start >= 0 && end > start, "uninstall stop block must precede hook removal");
  var block = source.slice(start, end);
  [false, true].forEach(function (modern) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-cli-stop-"));
    try {
      var called = path.join(dir, "called"), hook = path.join(dir, "hook-removed");
      exe(path.join(dir, "init_dts25.sh"),
        (modern ? ": 'lgroute-control'\n" : "") +
        'printf "%s" "$W25_PT" > "$CALLED"\nexit ' + (modern ? 7 : 0));
      var result = spawnSync("sh", ["-c", block + '\nprintf hook > "$HOOK_SENTINEL"', path.join(dir, "uninstall.sh")],
        {encoding: "utf8", timeout: 2000, env: Object.assign({}, process.env,
          {DTS_DEST: path.join(dir, "installed"), CALLED: called, HOOK_SENTINEL: hook})});
      assert.ifError(result.error);
      assert.equal(result.status, 1, result.stderr);
      assert.equal(fs.existsSync(called), modern, "outdated init must never execute");
      if (modern) assert.equal(fs.readFileSync(called, "utf8"), "stop");
      else assert.match(result.stdout, /The installed init script is outdated/);
      assert.equal(fs.existsSync(hook), false, "failed stop must not reach hook removal");
    } finally {
      fs.rmSync(dir, {recursive: true, force: true});
    }
  });
});

[
  ["restores a crash journal", "restore", 0],
  ["waits for stream release", "release", 0],
  ["active lock cannot masquerade as recovery", "active", 1],
  ["helper restore failure retains journal", "fail", 1],
  ["helper timeout retains journal", "timeout", 1],
  ["missing helper retains journal", "missing", 1],
  ["old experimental state is refused", "legacy", 1]
].forEach(function (scenario) {
  test("passthrough stop: " + scenario[0], function () {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-pt-stop-"));
    try {
      var runtime = path.join(dir, "run"), helper = path.join(dir, "lgroute-control");
      var called = path.join(dir, "called"), pid = path.join(runtime, "watcher.pid");
      var output = path.join(runtime, "sound-output"), journal = path.join(runtime, "lgroute.state");
      write(pid, "99999999\n");
      write(output, "99999999 external_arc auto\n");
      write(journal, "saved ALSA state");
      if (scenario[1] === "legacy") {
        write(path.join(runtime, "hdroute.state"), "unknown old graph");
        exe(path.join(dir, "hdroute.sh"), 'touch "' + called + '"');
      }
      if (scenario[1] !== "missing") exe(helper,
        '[ "$1" = --recover ] || exit 99\n' +
        '[ ! -e "' + output + '" ] && [ -e "' + runtime + '/stopping" ] || exit 98\n' +
        'echo recover >> "' + called + '"\n' +
        (scenario[1] === "fail" ? "exit 7" :
          scenario[1] === "active" ? "exit 0" :
          (scenario[1] === "release" ? '[ "$(wc -l < "' + called + '")" -ge 2 ] || exit 0\n' : "") +
          'rm -f "' + journal + '"'));
      var fn = initFn("w25_pt_stop").replace("/run/dts25", runtime)
        .replace("/var/lib/webosbrew/dts25/lgroute-control", helper)
        .split("/proc/").join(dir + "/proc/");
      var clock = path.join(dir, "clock");
      write(clock, "0\n");
      var script = [fn,
        'timeout() { [ "$1" = 5 ] || return 97; shift; ' +
          (scenario[1] === "timeout" ? 'return 124;' : '"$@";') + ' }',
        'sleep() { :; }',
        'date() { n=$(cat "' + clock + '"); echo "$n"; echo $((n + 1)) > "' + clock + '"; }',
        "w25_pt_stop"].join("\n");
      var result = sh(script);
      assert.ifError(result.error);
      assert.equal(result.status, scenario[2], result.stderr + result.stdout);
      assert.equal(fs.existsSync(output), false, "shutdown permission must be withdrawn even on failure");
      assert.equal(fs.existsSync(journal), scenario[2] !== 0, "failed recovery must retain journal");
      assert.equal(fs.existsSync(pid), scenario[2] !== 0, "failed stop must retain watcher bookkeeping");
      if (["missing", "legacy", "timeout"].includes(scenario[1])) assert.equal(fs.existsSync(called), false);
      else assert.match(fs.readFileSync(called, "utf8"), /^recover\n/);
      if (scenario[1] === "release") assert.equal(fs.readFileSync(called, "utf8"), "recover\nrecover\n");
      if (scenario[2]) assert.match(result.stdout, /^ERROR=/m);
    } finally {
      fs.rmSync(dir, {recursive: true, force: true});
    }
  });
});

test("failed passthrough and Disable calls report failure to the app", function () {
  var error = "Could not restore the eARC route; settings were kept.";
  return Promise.all(["setPassthrough", "disable"].map(function (method) {
    var mocked = loadService({
      detectProfile: function () { return Promise.resolve({profile: "webos25-armel-gst124", probes: {}}); },
      rootExec: function () { return Promise.resolve({stdout: "ERROR=" + error + "\n", stderr: ""}); },
      logActionResult: function () {}
    });
    return new Promise(function (resolve) {
      mocked.handlers[method]({payload: {enabled: false}, respond: resolve});
    }).then(function (response) {
      assert.equal(response.returnValue, false, method + " must not report success after a stop failure");
      assert.ok(response.errorText.includes(error), method + " must preserve the stop error");
    });
  }));
});

test("the recovery watchdog retries failures and stops with passthrough", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-recover-"));
  try {
    var helper = path.join(dir, "lgroute-control"), calls = path.join(dir, "calls");
    var flag = path.join(dir, "passthrough");
    write(flag);
    exe(helper, '[ "$1" = --recover ] || exit 99\necho recover >> "' + calls + '"\nexit 7');
    var fn = initFn("w25_pt_recover").split("/var/lib/webosbrew/dts25/lgroute-control").join(helper);
    var result = sh([
      'PT_FLAG="' + flag + '"', 'PT_RUN="' + dir + '"', fn,
      'timeout() { [ "$1" = 5 ] || return 99; shift; "$@"; }',
      'w25_log() { echo "$*"; }',
      'sleep() { [ "$1" = 1 ] || exit 99; [ "$(wc -l < "' + calls + '")" -lt 2 ] || rm -f "$PT_FLAG"; }',
      "w25_pt_recover"
    ].join("\n"));
    ok(result, "recovery watchdog");
    assert.equal(fs.readFileSync(calls, "utf8"), "recover\nrecover\n");
    assert.match(result.stdout, /journal retained/);
    write(flag);
    write(path.join(dir, "stopping"));
    ok(sh('PT_FLAG="' + flag + '"\nPT_RUN="' + dir + '"\n' + fn + '\nw25_pt_recover'), "stopped watchdog");
    assert.equal(fs.readFileSync(calls, "utf8"), "recover\nrecover\n", "shutdown must prevent more recovery calls");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("HD readiness uses the read-only ALSA check, not a route daemon", function () {
  var script = service.w25PassthroughSteps(true, true);
  assert.match(script, /timeout 5 .*lgroute-control --check/);
  assert.ok(!script.includes("hdroute.pid"));
  assert.ok(!script.includes("HD_RUNNING"));
  var source = fs.readFileSync(SERVICE, "utf8");
  assert.ok(source.includes('base.passthroughHdReady = base.passthroughToReceiver && kv.HD_READY === "1"'));
  assert.ok(!source.includes("passthroughHdRunning"));
});

/* Issue #13: the luna hub drops any message over 128 KiB and the app only sees
 * "Message not processed". 2.7.45's Enable script was 133,818 bytes, so Enable
 * failed on every TV. It must stay under the limit with room to grow, and the
 * boot script it writes must still come out byte for byte. */
test("the Enable call fits a luna message and writes the exact boot script", function () {
  var script = service.w25Enable(false);
  var size = Buffer.byteLength(JSON.stringify({ command: script }));
  assert.ok(size < 110 * 1024, "Enable exec payload is " + size + " bytes; the hub drops anything over 131072");
  var lines = script.split("\n");
  var start = lines.findIndex(function (l) { return /^base64 -d <<'B64EOF' \| gzip -dc > /.test(l); });
  assert.ok(start !== -1, "boot script heredoc not found");
  var end = lines.indexOf("B64EOF", start);
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-enable-"));
  var out = path.join(dir, "init.sh");
  var block = [lines[start].replace(/> "[^"]*"$/, '> "' + out + '"')].concat(lines.slice(start + 1, end + 1));
  var r = sh(block.join("\n"));
  assert.strictEqual(r.status, 0, r.stderr);
  assert.strictEqual(fs.readFileSync(out, "utf8"), service.w25InitScriptBody());
});

/* The generated script must resolve $APPBASE, not carry it as a literal.
 *
 * Regression for issue #1's actual cause: c2Q() single-quotes every config value,
 * which is correct for all of them except the payload path -- the one value that
 * deliberately contains a shell variable. Emitting C2_PAYLOAD='$APPBASE/payload/cx'
 * made the existence check test a path containing a dollar sign, so every C2 enable
 * failed as "payload file absent" and rolled back. No C2/G2 TV had ever completed
 * an enable because of it.
 *
 * Every other test builds the script with testOverrides, which substitutes a real
 * temp path and so never exercises the indirection. This one deliberately uses the
 * DEFAULT config. */
test("the default generated script expands $APPBASE in the payload path", function () {
  var script = service.c2Enable(true);

  var line = script.split("\n").filter(function (l) { return l.indexOf("C2_PAYLOAD=") === 0; })[0];
  assert.ok(line, "no C2_PAYLOAD assignment in the generated script");
  assert.ok(line.indexOf("'$APPBASE") === -1,
    "C2_PAYLOAD must not be single-quoted around $APPBASE: " + line);

  // Behavioral, not just textual: run the prelude plus the assignment and confirm
  // the value the script would actually use contains no unexpanded variable.
  var prelude = script.split("\n").slice(0, 30).filter(function (l) {
    return l.indexOf("APPBASE=") === 0 || l.indexOf("C2_PAYLOAD=") === 0;
  }).join("\n");
  var r = sh(prelude + '\nprintf "%s" "$C2_PAYLOAD"');
  assert.strictEqual(r.status, 0, "prelude failed: " + r.stderr);
  assert.ok(r.stdout.indexOf("$") === -1,
    "resolved payload path still contains a variable: " + r.stdout);
  assert.match(r.stdout, /\/payload\/cx$/);
});

/* A `refused` SHA verdict must say WHICH fault occurred.
 *
 * From issue #1: after an interrupted enable, every stock hash came back empty and
 * the app said only "SHA-256 is unavailable or returned an invalid digest". That one
 * sentence covers three different faults -- no hashing tool, unreadable targets, and
 * a malformed digest -- and because !hashable is evaluated before the foreign-bind
 * case it also masks a leftover bind. The owner had no way to act on it. */
test("the SHA refusal distinguishes no-tool, unreadable, and malformed", function () {
  var full = "a".repeat(64);

  var noTool = service.compatVerdict(C.C2, {C2_HASH_TOOL: "0", C2_GSTCOOL_SHA256: ""});
  assert.strictEqual(noTool.verdict, "refused");
  assert.match(noTool.verdictReason, /No working sha256sum/);

  // Tool present, targets unreadable: name them and point at the likely cause.
  var unreadable = service.compatVerdict(C.C2, {
    C2_HASH_TOOL: "1", C2_LIBAV_SHA256: "", C2_ISOMP4_SHA256: "",
    C2_MATROSKA_SHA256: "", C2_GSTCOOL_SHA256: ""
  });
  assert.strictEqual(unreadable.verdict, "refused");
  assert.strictEqual(unreadable.canForce, false);
  assert.match(unreadable.verdictReason, /Could not read these stock files/);
  assert.match(unreadable.verdictReason, /libgstlibav\.so/);
  assert.match(unreadable.verdictReason, /libgstmatroska\.so/);
  assert.match(unreadable.verdictReason, /reboot/i);

  // A digest that is present but not 64 hex chars is a different fault, and must
  // NOT be reported as an unreadable file.
  var malformed = service.compatVerdict(C.C2, {
    C2_HASH_TOOL: "1", C2_LIBAV_SHA256: full, C2_ISOMP4_SHA256: full,
    C2_MATROSKA_SHA256: full, C2_GSTCOOL_SHA256: "zz"
  });
  assert.strictEqual(malformed.verdict, "refused");
  assert.match(malformed.verdictReason, /invalid digest/);
  assert.ok(malformed.verdictReason.indexOf("Could not read") === -1,
    "a malformed digest must not be reported as an unreadable file");
});

/* Every generated C2 script must define $APPBASE before using it.
 *
 * All of them run under `set -u`, and only two of the six that embed the C2 vars
 * prepended APPBASE_PRELUDE. When C2_PAYLOAD started expanding $APPBASE (2.7.7),
 * DETECT_PROBE and the boot hook died on that assignment line and every line after
 * it silently never ran -- the C2 fields came back empty and the app refused with
 * "no working sha256sum". 2.7.7 was verified against the enable script only, which
 * was one of the two that happened to be correct. */
test("every generated C2 script defines $APPBASE before using it", function () {
  var scripts = {
    DETECT_PROBE: service.DETECT_PROBE,
    c2Enable: service.c2Enable(true),
    c2Disable: service.c2Disable(false),
    c2InitScriptBody: service.c2InitScriptBody(),
    c2StatusProbe: service.c2StatusProbe(),
    c2SelfTest: service.c2SelfTest()
  };
  Object.keys(scripts).forEach(function (name) {
    var lines = scripts[name].split("\n");
    var use = lines.findIndex(function (l) {
      return l.indexOf("$APPBASE") >= 0 && l.indexOf("APPBASE=/media") !== 0;
    });
    if (use === -1) return;                       // does not use it: nothing to prove
    var def = lines.findIndex(function (l) { return l.indexOf("APPBASE=") === 0; });
    assert.notStrictEqual(def, -1, name + " uses $APPBASE but never defines it");
    assert.ok(def < use,
      name + " defines APPBASE at line " + def + " but uses it at " + use);
  });

  // Behavioral: the probe must run to completion under `set -u` on a machine with
  // none of the TV's tools, and still emit its C2 fields. An unbound variable would
  // abort it mid-script with a zero exit from the last successful echo.
  var r = sh(service.DETECT_PROBE);
  assert.strictEqual(r.status, 0, "probe exited " + r.status + ": " + r.stderr);
  assert.ok(r.stderr.indexOf("unbound variable") === -1,
    "probe hit an unbound variable: " + r.stderr);
  ["C2_HASH_TOOL=", "C2_OWNED=", "C2_FOREIGN=", "C2_MOUNT_LIBAV="].forEach(function (k) {
    assert.ok(r.stdout.indexOf("\n" + k) >= 0 || r.stdout.indexOf(k) === 0,
      "probe did not emit " + k + " (it aborted before the C2 block)");
  });
});

/* The optional TS demuxer must never cost a working install.
 *
 * The C2 profile works today with four 1.14.4 binaries and MP4/MKV. Adding a fifth
 * (LG 1.14.4 libgstmpegtsdemux.so) introduces two NEEDED libraries whose presence on
 * a C2/G2/CS is unverified. c2_payload requires its mandatory files to copy AND trace,
 * so a hard fifth file would fail the whole enable and take away working MP4 on any
 * TV lacking those libs. It must degrade instead. */
test("a failing TS demuxer degrades to MP4/MKV instead of failing the enable", function () {
  var TSF = "libgstmpegtsdemux.so";

  // 1. TS not shipped at all (an older .ipk, or the payload dir absent).
  var f = fixture(), o = f.overrides();
  var r = run(f, service.c2Enable(true, o));
  ok(r, "enable with no TS payload");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
  assert.equal(fs.existsSync(path.join(f.state, "owner")), true,
    "the working four must still produce an owned install");

  // 2. TS shipped, stock target present, but its dependencies do not resolve here.
  //    This is the exact risk carried in DESIGN-C2-TS-DEMUX.md.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  write(path.join(f.target, TSF), "stock-" + TSF);
  r = run(f, service.c2Enable(true, o), {TS_MISS_DEP: "1"});
  ok(r, "enable with an unresolvable TS demuxer");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
  assert.match(r.stdout, /TS_SKIP=/);
  assert.equal(fs.existsSync(path.join(f.state, "owner")), true,
    "a bad TS demuxer must not roll back the working install");
  assert.equal(fs.existsSync(path.join(f.gst, TSF)), false,
    "the unusable TS file must not be left staged");
  assert.equal(fs.existsSync(path.join(f.state, "trace." + TSF)), true);
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall with only the skipped TS trace left");
  assert.match(r.stdout, /^OK$/m);
  assert.equal(fs.existsSync(f.state), false);

  // 3. TS shipped, traces clean, stock target present -> bound.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  write(path.join(f.target, TSF), "stock-" + TSF);
  r = run(f, service.c2Enable(true, o));
  ok(r, "enable with a usable TS demuxer");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=1/);
  // #22: the staged TS demuxer and its trace are ours, so Uninstall must remove
  // them rather than refuse the whole state as unexpected.
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall with the TS demuxer staged");
  assert.doesNotMatch(r.stdout, /REFUSED/, r.stdout);
  assert.equal(fs.existsSync(f.state), false, "uninstall must remove the dedicated C2 state");

  // 4. TS shipped and usable, but the TV has no stock TS demuxer to override.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  r = run(f, service.c2Enable(true, o));
  ok(r, "enable with no stock TS target");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
});

test("C2 uninstall after a firmware upgrade leaves the new rootfs and webOS 25 state alone", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  function firmware(fw, webos, gst, hashes) {
    exe(f.command("nyx-cmd"), 'case "$*" in *hardware_id) echo HE_DTV_W22O_AFABATPU;; *product_id) echo OLED55CS6LA;; *board_type) echo O22_DVB;; *webos_manufacturing_version) echo ' + fw + ';; *webos_release) echo ' + webos + ';; esac');
    exe(f.command("gst-inspect-1.0"), '[ "$1" = --version ] && { echo "GStreamer ' + gst + '"; exit 0; }; if [ -n "${GST_REGISTRY_1_0:-}" ] && [ "$#" -eq 0 ]; then printf registry > "$GST_REGISTRY_1_0" || exit 1; fi; exit 0');
    exe(f.command("sha256sum"), 'case "$1" in *libgstlibav.so) h="' + hashes.libav + '";; *libgstisomp4.so) h="' + hashes.iso + '";; *libgstmatroska.so) h="' + hashes.mkv + '";; *gstcool.conf) h="' + "a".repeat(64) + '";; *) exec /usr/bin/shasum -a 256 "$1";; esac; echo "$h  $1"');
  }
  firmware("23.25.55", "9.2.2", "1.18.5", C.sets[1]);
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare the pre-upgrade C2 install");
  assert.match(r.stdout, /TS_BOUND=1/);
  assert.match(fs.readFileSync(o.baseline, "utf8"), /firmware=23\.25\.55\n/);

  write(f.mountinfo, "1 0 0:1 / / rw - fake root rw\n");
  fs.unlinkSync(path.join(f.target, "libgstisomp4_1_8.so"));
  var untouched = [f.gstcool, f.registry];
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", ts].forEach(function (name) {
    var file = path.join(f.target, name);
    write(file, "upgraded-stock-" + name);
    untouched.push(file);
  });
  ["dts25/libgstdtsdec.so", "truehd/libgstlibav.so", "demux25/libgstisomp4.so", "init.d/restore_dts25"].forEach(function (name) {
    var file = path.join(f.dir, name);
    write(file, "webos25-" + name);
    untouched.push(file);
  });
  var before = untouched.map(function (file) { return fs.readFileSync(file, "utf8"); });
  firmware("33.31.68", "10.3.1", "1.24.0", {
    libav: "061df7c4d219f3a9e03b4888ee09e9e10fda79b2ce40f916d14d39a225af2697",
    iso: "8450bd3efe8055d1f37875fab33e489ce268a60c5e13aa907da9c283dec16d62",
    mkv: "986bb059463d40f8d5581289a390a4576786bf2d339a0a61dc94482b5d426209"
  });
  assert.equal(service.c2OwnerRoute("enable", "webos25-armel-gst124", {verdict: "drift"}), "refuse");
  assert.equal(service.c2OwnerRoute("uninstall", "webos25-armel-gst124", {verdict: "drift"}), "uninstall");
  r = run(f, 'exec sh "' + o.init + '"');
  ok(r, "boot after the firmware upgrade");
  assert.match(r.stdout, /REFUSED=recovery/);
  assert.match(r.stdout, /persisted identity or hashes drifted/);
  assert.equal(fs.readFileSync(f.mountinfo, "utf8"), "1 0 0:1 / / rw - fake root rw\n");
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall the stale C2 install");
  assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.equal(fs.existsSync(f.state), false);
  assert.equal(fs.existsSync(o.hook), false);
  untouched.forEach(function (file, i) { assert.equal(fs.readFileSync(file, "utf8"), before[i], file); });
});

test("C2 uninstall rejects TS symlinks and directories before mutation", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare TS state safety checks");
  assert.match(r.stdout, /TS_BOUND=1/);
  var before = fs.readFileSync(f.mountinfo, "utf8"), sentinel = path.join(f.dir, "unrelated");
  write(sentinel, "untouched");
  [path.join(f.gst, ts), path.join(f.state, "trace." + ts)].forEach(function (file) {
    var content = fs.readFileSync(file);
    ["symlink", "dangling symlink", "directory"].forEach(function (kind) {
      fs.unlinkSync(file);
      if (kind === "directory") fs.mkdirSync(file);
      else fs.symlinkSync(kind === "symlink" ? sentinel : sentinel + ".missing", file);
      r = run(f, service.c2Disable(true, o));
      ok(r, kind + " at " + file);
      assert.match(r.stdout, /unexpected entries or symlinks/);
      assert.equal(fs.readFileSync(f.mountinfo, "utf8"), before);
      assert.equal(fs.existsSync(o.recovery), false);
      assert.equal(fs.existsSync(o.owner), true);
      assert.equal(fs.existsSync(o.hook), true);
      assert.equal(fs.readFileSync(sentinel, "utf8"), "untouched");
      if (kind === "directory") fs.rmdirSync(file); else fs.unlinkSync(file);
      write(file, content);
    });
  });
});

test("C2 TS cleanup failures retain recovery state and permit a checked retry", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare TS cleanup failures");
  assert.match(r.stdout, /TS_BOUND=1/);
  [path.join(f.gst, ts), path.join(f.state, "trace." + ts)].forEach(function (file) {
    r = run(f, service.c2Disable(true, o), {FAIL_RM_PATH: file});
    ok(r, "refuse failed cleanup of " + file);
    assert.match(r.stdout, /REFUSED=recovery/);
    assert.match(r.stdout, /checked C2 cleanup failed/);
    assert.doesNotMatch(r.stdout, /^OK$/m);
    [o.owner, o.baseline, o.recovery, o.init, o.env, o.hook].forEach(function (kept) {
      assert.equal(fs.existsSync(kept), true, kept);
    });
  });
  r = run(f, service.c2Disable(true, o));
  ok(r, "retry TS cleanup");
  assert.match(r.stdout, /^OK$/m);
  assert.equal(fs.existsSync(f.state), false);
  assert.equal(fs.existsSync(o.hook), false);
});

/* Every self-test case the C2 shell can emit must be mapped by the handler.
 *
 * check-kv-sync.js covers the UPPER_SNAKE probe keys, but cannot judge the short
 * lowercase self-test keys (`mp4`, `ts`, `m2ts`) because those substrings appear all
 * over service.js and app.js. That is exactly the gap the 2.7.14 bug fell through:
 * the shell ran ts/m2ts and the handler's file map listed only mp4, so the results
 * were discarded and the UI showed "-" while all three clips actually played. */
test("the C2 self-test handler maps every case its shell can emit", function () {
  var shell = service.c2SelfTest();

  // Cases the shell can produce: the literal mp4 echo plus the `for c in ...` loop.
  var emitted = {};
  (shell.match(/echo\s+"([a-z0-9]+)=(?:PASS|FAIL)/g) || []).forEach(function (m) {
    emitted[m.replace(/echo\s+"/, "").replace(/=.*/, "")] = true;
  });
  var loop = shell.match(/for\s+c\s+in\s+([a-z0-9 ]+);\s*do/);
  if (loop) loop[1].trim().split(/\s+/).forEach(function (w) { emitted[w] = true; });

  assert.ok(emitted.mp4, "the self-test must still cover mp4");
  assert.ok(emitted.ts && emitted.m2ts,
    "the self-test shell should emit ts and m2ts; got " + Object.keys(emitted).join(","));

  // The handler's file map is the consumer. Read it out of the source rather than
  // duplicating the list here, so the test tracks the real mapping.
  var src = fs.readFileSync(SERVICE, "utf8");
  var mapLine = src.match(/var files = \{([^}]*)\};/);
  assert.ok(mapLine, "could not find the self-test file map in the handler");
  var mapped = {};
  (mapLine[1].match(/([a-z0-9]+)\s*:/g) || []).forEach(function (k) {
    mapped[k.replace(/\s*:$/, "")] = true;
  });

  Object.keys(emitted).forEach(function (k) {
    assert.ok(mapped[k],
      "the C2 self-test shell emits '" + k + "=' but the handler's file map does not " +
      "list it, so that result is silently discarded (this is the 2.7.14 bug)");
  });
});
