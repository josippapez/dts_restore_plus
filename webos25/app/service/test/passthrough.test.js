"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, sh, write, exe, run, ok, initFn, loadService, ROOT, SERVICE} = require("./helpers");

/* Issue #12: the passthrough toggle. dtsdec only sends the bitstream to
 * external_arc or external_optical with Pass Through or Auto; on speakers it
 * would be loud noise. */
test("the passthrough toggle only reports a receiver for ARC/eARC or optical", function () {
  assert.strictEqual(service.w25PtReceiver("external_arc passThrough"), true);
  assert.strictEqual(service.w25PtReceiver("external_arc auto"), true);
  assert.strictEqual(service.w25PtReceiver("external_arc pcm"), false);
  assert.strictEqual(service.w25PtReceiver("external_optical passThrough"), true);
  assert.strictEqual(service.w25PtReceiver("external_optical auto"), true);
  assert.strictEqual(service.w25PtReceiver("external_optical pcm"), false);
  // Optical carries only the core: the HD carrier stays eARC-only.
  assert.strictEqual(service.w25HdOutput("external_optical passThrough"), false);
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
  assert.ok(body.includes("\nw25_pt_stop_soft\nw25_pt_start\n"),
    "boot apply must stop passthrough before restarting it after Disable");
  assert.ok(initFn("w25_stand_down").includes("w25_pt_stop_soft"), "stand-down must not stall on a failed stop");
  assert.ok(initFn("w25_self_heal").includes("w25_pt_stop_soft"), "self-heal must not stall on a failed stop");
  assert.ok(body.includes("stop) w25_pt_stop || exit 1; w25_kmod_stop || exit 1 ;;"), "the app toggle keeps failing hard");
  assert.ok(!body.includes("--recover"), "HD never writes LG's mixer, so there is nothing to recover");
  assert.ok(!body.includes("hdroute.sh"));
  assert.ok(service.w25PassthroughSteps(true, true).includes("/lgroute-control ]"));
  assert.ok(!service.w25PassthroughSteps(true, true).includes("HD_RUNNING"));
  var off = service.w25PassthroughSteps(false);
  var offStop = off.indexOf("\nW25_PT=stop ");
  assert.ok(offStop >= 0 && offStop < off.indexOf('rm -f "$FLAG"'), "stop before clearing the passthrough marker");
  var disable = service.w25Disable();
  var stop = disable.indexOf("\nw25_pt_stop_soft\n");
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

test("CLI uninstall never runs an outdated init and continues past a failed stop", function () {
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
        (modern ? "W25_GATE_VERSION=35\n" : "W25_GATE_VERSION=34\n") +
        'printf "%s" "$W25_PT" > "$CALLED"\nexit ' + (modern ? 7 : 0));
      var result = spawnSync("sh", ["-c", 'log() { echo "$*"; }' + block + '\nprintf hook > "$HOOK_SENTINEL"', path.join(dir, "uninstall.sh")],
        {encoding: "utf8", timeout: 2000, env: Object.assign({}, process.env,
          {DTS_DEST: path.join(dir, "installed"), CALLED: called, HOOK_SENTINEL: hook})});
      assert.ifError(result.error);
      assert.equal(result.status, 0, result.stderr);
      assert.equal(fs.existsSync(called), modern, "outdated init must never execute");
      if (modern) {
        assert.equal(fs.readFileSync(called, "utf8"), "stop");
        assert.match(result.stdout, /WARN passthrough stop failed/);
      }
      assert.equal(fs.existsSync(hook), true, "a failed stop must not block uninstall");
    } finally {
      fs.rmSync(dir, {recursive: true, force: true});
    }
  });
});

test("passthrough stop withdraws permission and clears watcher state", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-pt-stop-"));
  try {
    var runtime = path.join(dir, "run"), pid = path.join(runtime, "watcher.pid");
    var output = path.join(runtime, "sound-output");
    write(pid, "99999999\n");
    write(output, "99999999 external_arc passThrough\n");
    var fn = initFn("w25_pt_stop").replace("/run/dts25", runtime).split("/proc/").join(dir + "/proc/");
    var result = sh([fn, "w25_pt_stop"].join("\n"));
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stderr + result.stdout);
    assert.equal(fs.existsSync(output), false, "shutdown permission must be withdrawn");
    assert.equal(fs.existsSync(pid), false);
    assert.equal(fs.existsSync(path.join(runtime, "stopping")), true);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("soft stop logs and reports a failed stop without failing", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-soft-stop-"));
  try {
    var log = path.join(dir, "log");
    var result = sh([
      'w25_log() { echo "$*" >> "' + log + '"; }',
      "w25_pt_stop() { echo 'ERROR=The passthrough watcher did not stop; settings were kept.'; return 1; }",
      initFn("w25_pt_stop_soft"), "w25_pt_stop_soft"].join("\n"));
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stdout, "WARN_PT_STOP=The passthrough watcher did not stop; settings were kept.\n");
    assert.match(fs.readFileSync(log, "utf8"), /WARN passthrough stop failed/);
    var clean = sh(["w25_pt_stop() { :; }", initFn("w25_pt_stop_soft"), "w25_pt_stop_soft"].join("\n"));
    assert.equal(clean.status, 0);
    assert.equal(clean.stdout, "");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("Disable succeeds with a warning when passthrough did not stop cleanly", function () {
  var mocked = loadService({
    detectProfile: function () { return Promise.resolve({profile: "webos25-armel-gst124", probes: {}}); },
    rootExec: function () { return Promise.resolve({stdout: "WARN_PT_STOP=stuck\nOK\n", stderr: ""}); },
    logActionResult: function () {}
  });
  return new Promise(function (resolve) {
    mocked.handlers.disable({payload: {}, respond: resolve});
  }).then(function (response) {
    assert.equal(response.returnValue, true, response.errorText);
    assert.match(response.warning, /Passthrough did not stop cleanly \(stuck\)/);
  });
});

test("failed passthrough and Disable calls report failure to the app", function () {
  var error = "The passthrough watcher did not stop; settings were kept.";
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

test("HD readiness uses the read-only ALSA check, not a route daemon", function () {
  var script = service.w25PassthroughSteps(true, true);
  assert.match(script, /timeout 5 .*lgroute-control --check/);
  assert.ok(!script.includes("hdroute.pid"));
  assert.ok(!script.includes("HD_RUNNING"));
  var source = fs.readFileSync(SERVICE, "utf8");
  assert.ok(source.includes('base.passthroughHdReady = base.passthroughRequested && w25HdOutput(kv.PT_OUT) && kv.HD_READY === "1"'));
  assert.ok(!source.includes("passthroughHdRunning"));
});

test("HD readiness distinguishes sink builds, helper and eARC failures", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-hd-ready-"));
  try {
    var script = service.w25HdReadinessSteps()
      .split("/run/dts25").join(dir)
      .split("/var/lib/webosbrew/dts25").join(dir);
    assert.match(sh(script).stdout, /HD_REASON=untested-sink\nHD_READY=0/);
    write(path.join(dir, "sink-ok"));
    assert.match(sh(script).stdout, /HD_REASON=missing-helper\nHD_READY=0/);
    exe(path.join(dir, "lgroute-control"), "exit 1");
    var bounded = 'timeout() { shift; "$@"; }\n' + script;
    assert.match(sh(bounded).stdout, /HD_REASON=earc-unavailable\nHD_READY=0/);
    exe(path.join(dir, "lgroute-control"), "exit 0");
    assert.match(sh(bounded).stdout, /HD_REASON=no-module\nHD_READY=0/);
    assert.match(service.w25HdReason("no-module", "external_arc passThrough"), /kernel module is not loaded/);
    var loaded = bounded.split("/sys/module/earc_route").join(dir);
    assert.match(sh(loaded).stdout, /HD_REASON=ready\nHD_READY=1/);
    assert.match(service.w25HdReason("untested-sink", "external_arc passThrough"), /tested list/);
    assert.equal(service.w25HdReason("ready", "external_arc passThrough"), "");
    assert.match(service.w25HdReason("ready", "tv_speaker pcm"), /Pass Through/);
    assert.match(service.w25HdReason("ready", "external_arc auto"), /Advanced Settings > Digital Sound Output to Pass Through/);
    // On optical the readiness probe reports earc-unavailable, never ready.
    assert.match(service.w25HdReason("earc-unavailable", "external_optical passThrough"), /needs eARC/);
    assert.match(service.w25HdReason("untested-sink", "external_optical auto"), /needs eARC/);
    ok(spawnSync("sh", ["-n"], {input: service.w25PassthroughSteps(true, true), encoding: "utf8"}), "HD toggle syntax");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("HD can only be turned on in eARC Pass Through", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-hd-mode-"));
  try {
    exe(path.join(dir, "lgroute-control"), "exit 0");
    var lines = service.w25PassthroughSteps(true, true).split("\n");
    var guard = lines.slice(1, 4).join("\n").split("/run/dts25").join(dir)
      .split("/var/lib/webosbrew/dts25").join(dir) + "\necho PASSED";
    [["auto", false], ["pcm", false], ["passThrough", true]].forEach(function (c) {
      write(path.join(dir, "sound-output"), "1 external_arc " + c[0] + "\n");
      var r = sh(guard);
      assert.equal(/PASSED/.test(r.stdout), c[1], c[0] + ": " + r.stdout);
      if (!c[1]) assert.match(r.stdout, /ERROR=.*Pass Through/);
    });
    ["passThrough", "auto"].forEach(function (mode) {
      write(path.join(dir, "sound-output"), "1 external_optical " + mode + "\n");
      var r = sh(guard);
      assert.doesNotMatch(r.stdout, /PASSED/, "optical " + mode);
      assert.match(r.stdout, /ERROR=DTS-HD needs eARC/);
    });
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

/* Discussion #17: 2.60 allowlisted the G2 33.22.95 sink for DTS-HD but not its
 * plugins, so Enable still said "Unsupported TV". */
test("the verified set covers the G2 on firmware 33.22.95", function () {
  var script = [
    initFn("w25_fp_differs"), initFn("w25_verdict"),
    "w25_fp_get() { :; }", "w25_product_id() { echo OLED55G26LA; }",
    "GST_MM_NOW=1.24 M_LIBAV=ff330d3578ce83da402d686f4b5ad5c4",
    "M_ISOMP4=09d6385016be02c4552e719f3a01ef52 M_MPEGTS=b0380d3c66d382c15504b81b09ff94bd",
    "w25_verdict", 'echo "$VERDICT|$LABEL"'
  ].join("\n");
  var r = sh(script);
  assert.strictEqual(r.stdout.trim(), "verified|LG G2 OLED55G26LA (webOS 10.2.2, GStreamer 1.24.0)", r.stderr);
});

test("HD sink allowlist accepts exact tested hashes only", function () {
  var fn = initFn("w25_pt_sink_verified");
  var known = "90dba3ef32704ce4096bf1e6045d2879d0c2192d507bc04d1e8e98437f420a43";
  ok(sh(fn + '\nw25_pt_sink_verified "' + known + '"'), "known sink");
  var g2 = "792caa7e048a8550ec3c67e0fd4c9d94684d0633e0ff250a2bcbd0e29d538fe9"; // G2 33.22.95
  ok(sh(fn + '\nw25_pt_sink_verified "' + g2 + '"'), "G2 33.22.95 sink");
  var g3 = "21ad477c010fc6cb2e6989ff263892981b9d26d28cd83d1736edc3331bf80745"; // G3 33.31.68
  ok(sh(fn + '\nw25_pt_sink_verified "' + g3 + '"'), "G3 33.31.68 sink");
  ["", "x", known.slice(1), "a".repeat(64)].forEach(function (hash) {
    assert.notEqual(sh(fn + '\nw25_pt_sink_verified "' + hash + '"').status, 0);
  });
});

/* Testers without SSH photograph the newest route log lines from the app. */
test("the receiver log shows the last 80 route log lines and nothing else", function () {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-routelog-"));
  var lines = [];
  for (var i = 1; i <= 90; i++) lines.push("line " + i);
  write(path.join(dir, "lgroute.log"), lines.join("\n") + "\n");
  var script = service.w25RouteLogSteps();
  assert.match(script, /^tail -n 80 \/run\/dts25\/lgroute\.log /);
  var r = sh(script.replace("/run/dts25", dir));
  assert.strictEqual(r.status, 0);
  assert.strictEqual(r.stdout, lines.slice(10).join("\n") + "\n");
  // No log yet: empty output, still a success.
  r = sh(script.replace("/run/dts25", path.join(dir, "missing")));
  assert.strictEqual(r.status, 0);
  assert.strictEqual(r.stdout, "");
});
