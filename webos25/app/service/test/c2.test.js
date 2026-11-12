"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, C, sh, write, fixture, run, ok} = require("./helpers");

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

test("copy, write-commit, hook, config, and cleanup command faults execute fail-closed", function () {
  [["FAIL_CP", /payload|cleanup/], ["FAIL_MV", /payload|baseline|cleanup|state/], ["FAIL_CHMOD", /init|hook|rollback|recovery/], ["FAIL_SED", /config|recovery/]].forEach(function (scenario) {
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
  // The payload is staged through a rename too, so FAIL_MV above stops there; the
  // init/baseline commit rename still has to fail closed on its own.
  f = fixture(); o = f.overrides();
  r = run(f, service.c2Enable(true, o), {FAIL_MV_PATH: o.init}); ok(r, "init commit failure");
  assert.match(r.stdout, /baseline|cleanup|state/);
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
