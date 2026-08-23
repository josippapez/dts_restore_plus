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

function loadService() {
  var source = fs.readFileSync(SERVICE, "utf8") + "\nmodule.exports = {" +
    "compatVerdict: compatVerdict, isKnownProfile: isKnownProfile," +
    "c2OwnerRoute: c2OwnerRoute," +
    "c2Config: c2Config, c2InitScriptBody: c2InitScriptBody," +
    "c2Enable: c2Enable, c2Disable: c2Disable, c2StatusProbe: c2StatusProbe," +
    "c2StatusBindsComplete: c2StatusBindsComplete, c2SelfTest: c2SelfTest, constants: {C2: PROFILE_C2," +
    "sets: C2_EXPECTED_SETS, matchSet: c2MatchSet," +
    "libav: C2_EXPECTED_SETS[0].libav, iso: C2_EXPECTED_SETS[0].iso, mkv: C2_EXPECTED_SETS[0].mkv}};";
  var FakeService = function () { this.register = function () {}; };
  var context = {
    require: function (name) { return name === "webos-service" ? FakeService : require(name); },
    module: {exports: {}}, exports: {}, Buffer: Buffer, console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout
  };
  vm.runInNewContext(source, context, {filename: SERVICE});
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
  exe(path.join(bin, "loader"), '[ "${FAIL_TRACE:-}" = 1 ] && { echo "not found"; exit 1; }; exit 0');
  [["cp", "/bin/cp"], ["rm", "/bin/rm"], ["rmdir", "/bin/rmdir"], ["mkdir", "/bin/mkdir"], ["mv", "/bin/mv"], ["chmod", "/bin/chmod"], ["ln", "/bin/ln"], ["sed", "/usr/bin/sed"]].forEach(function (entry) {
    exe(path.join(bin, entry[0]), '[ "${FAIL_' + entry[0].toUpperCase() + ':-}" = 1 ] && exit 1; [ -n "${FAIL_' + entry[0].toUpperCase() + '_PATH:-}" ] && { for a in "$@"; do [ "$a" = "$FAIL_' + entry[0].toUpperCase() + '_PATH" ] && exit 1; done; }; exec ' + entry[1] + ' "$@"');
  });
  exe(path.join(bin, "mount"), 'if [ "${FAIL_MOUNT:-}" = 1 ]; then case "${INJECT_STATE_ENTRY:-}" in file) printf foreign > "' + state + '/foreign-entry";; hidden) printf foreign > "' + state + '/.foreign-hidden";; dir) /bin/mkdir "' + state + '/foreign-dir";; symlink) /bin/ln -s "' + dir + '" "' + state + '/foreign-link";; esac; exit 1; fi; target=""; source=""; for a in "$@"; do source="$target"; target="$a"; done; printf "2 1 0:1 %s %s rw - fake fake rw\n" "$source" "$target" >> "' + mountinfo + '"');
  exe(path.join(bin, "umount"), '[ "${FAIL_UMOUNT:-}" = 1 ] && exit 1; target=""; for a in "$@"; do target="$a"; done; tmp="' + mountinfo + '.tmp"; grep -v " $target " "' + mountinfo + '" > "$tmp"; mv "$tmp" "' + mountinfo + '"');
  return {dir: dir, bin: bin, state: state, gst: gst, target: target, payload: payload,
    mountinfo: mountinfo, registry: registry, hook: path.join(dir, "hook"), legacy: path.join(dir, "legacy"),
    init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"),
    registrySource: path.join(state, "registry.bin"), gstcool: path.join(dir, "etc/gstcool.conf"),
    core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"),
    command: function (name) { return path.join(bin, name); },
    overrides: function () { return {state: state, gst: gst, owner: path.join(state, "owner"), baseline: path.join(state, "baseline"), recovery: path.join(state, "recovery"), init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"), registrySource: path.join(state, "registry.bin"), hook: path.join(dir, "hook"), legacyHook: path.join(dir, "legacy"), payload: payload, mountinfo: mountinfo, gstTarget: target, gstcool: path.join(dir, "etc/gstcool.conf"), core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"), inspect: path.join(bin, "gst-inspect-1.0"), mount: path.join(bin, "mount"), umount: path.join(bin, "umount"), cp: path.join(bin, "cp"), rm: path.join(bin, "rm"), rmdir: path.join(bin, "rmdir"), mkdir: path.join(bin, "mkdir"), mv: path.join(bin, "mv"), chmod: path.join(bin, "chmod"), ln: path.join(bin, "ln"), readlink: "/usr/bin/readlink", sed: path.join(bin, "sed")}; }
  };
}
function run(f, script, extra) { return sh(script, Object.assign({PATH: f.bin + ":" + process.env.PATH, GST_REGISTRY_1_0: f.registry}, extra || {})); }
function ok(result, message) { assert.equal(result.status, 0, message + "\n" + result.stderr + "\n" + result.stdout); }
function test(name, fn) { try { fn(); console.log("ok - " + name); } catch (e) { console.error("not ok - " + name + "\n" + e.stack); process.exitCode = 1; } }

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
