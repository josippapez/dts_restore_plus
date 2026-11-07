"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, C, sh, write, fixture, run, ok} = require("./helpers");

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
