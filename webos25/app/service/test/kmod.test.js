"use strict";

/* earc_route held loaded for DTS-HD passthrough (webos25/kmod). */
var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, sh, write, exe, ok, initFn, loadService, ROOT} = require("./helpers");

var body = service.w25InitScriptBody();
function oneLine(name) {
  var l = body.split("\n").filter(function (x) { return x.indexOf(name + "() { ") === 0; });
  assert.strictEqual(l.length, 1, name);
  return l[0];
}
function tmp(prefix) { return fs.mkdtempSync(path.join(os.tmpdir(), prefix)); }
function read(file) { return fs.existsSync(file) ? fs.readFileSync(file, "utf8") : null; }

test("the module runs on the 5.4.268 and 6.12 kernels, beside no enabled DTS tool, with its payload", function () {
  var dir = tmp("dts-kmod-refusal-");
  try {
    var kmod = path.join(dir, "hdmi"), core = path.join(dir, "lg-dts-core"), gh = path.join(dir, "lg-dts-native");
    ["earc_route-5.4.ko", "earc_route-6.12.ko", "earc-route-load.sh"].forEach(function (f) { write(path.join(kmod, f), "x"); });
    var fn = initFn("w25_kmod_refusal").split("/var/lib/lg-dts-core").join(core).split("/var/lib/lg-dts-native").join(gh);
    function refusal(kernel) {
      var r = sh(['uname() { echo "' + kernel + '"; }', "KMOD_DIR=" + kmod, fn, 'printf "[%s]" "$(w25_kmod_refusal)"'].join("\n"));
      assert.strictEqual(r.status, 0, r.stderr);
      return r.stdout;
    }
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[]");
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.2"), "[]");
    assert.strictEqual(refusal("6.12.44-352"), "[]");
    assert.strictEqual(refusal("6.6.1"), "[kernel]");
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.9"), "[kernel]");
    fs.unlinkSync(path.join(kmod, "earc_route-6.12.ko"));
    assert.strictEqual(refusal("6.12.44-352"), "[payload]", "each kernel needs its own build");
    write(path.join(gh, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[glasshouse]");
    write(path.join(core, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[lg-dts-core]");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("a process counts as alive until it is a zombie", function () {
  var dir = tmp("dts-kmod-alive-");
  try {
    write(path.join(dir, "proc/100/stat"), "100 (sh) S 1 100");
    write(path.join(dir, "proc/101/stat"), "101 (sh) Z 1 101");
    var fn = oneLine("w25_alive").split("/proc/").join(dir + "/proc/");
    var r = sh([fn, "for p in 100 101 102 ''; do w25_alive \"$p\" && echo \"$p alive\" || echo \"$p gone\"; done"].join("\n"));
    assert.strictEqual(r.stdout, "100 alive\n101 gone\n102 gone\n gone\n");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

/* The holder and its stop, with a fake loader that "loads" the module by creating
 * its sysfs directory, and an rmmod that removes it, or fails (exit 20). */
function holder(rmmodFails, loadFails) {
  var dir = tmp("dts-kmod-hold-");
  var kmod = path.join(dir, "hdmi"), run = path.join(dir, "run"), module = path.join(dir, "module");
  var pt = path.join(dir, "passthrough"), hd = path.join(dir, "passthrough-hd");
  fs.mkdirSync(run);
  write(pt, ""); write(hd, "");
  // Records whether the crash marker covered the load.
  var during = '[ -f "' + path.join(kmod, "loading") + '" ] && : > "' + path.join(dir, "marked-during-load") + '"; ';
  exe(path.join(kmod, "earc-route-load.sh"), during + (loadFails
    ? 'echo "earc-route: REFUSED: test"; exit 1'
    : 'mkdir -p "' + module + '"; echo "earc-route: PASSTHROUGH ACTIVE (mode=7) EXPERIMENTAL"'));
  var vars = ["KMOD_DIR=" + kmod, "KMOD_PID=" + run + "/hdmi.pid", "KMOD_OUT=" + run + "/hdmi-watch.out",
    "KMOD_EXIT=" + run + "/hdmi-watch.exit", "KMOD_MODULE=" + module, "PT_FLAG=" + pt, "HD_FLAG=" + hd,
    oneLine("w25_module_wanted"), "w25_log() { :; }",
    rmmodFails ? "rmmod() { echo 'rmmod: busy' >&2; return 1; }" : 'rmmod() { rm -rf "' + module + '"; }',
    // macOS has no /proc: the same zombie rule, from ps.
    'w25_alive() { [ -n "$1" ] || return 1; s=$(ps -o stat= -p "$1" 2>/dev/null) || return 1; case $s in Z*|"") return 1 ;; esac; }'];
  var hold = path.join(dir, "hold.sh");
  // Polls every 0.1 s here instead of 10 s.
  write(hold, vars.concat([initFn("w25_kmod_hold").replace("sleep 10", "sleep 0.1"), "w25_kmod_hold"]).join("\n"));
  var stop = vars.concat([initFn("w25_kmod_stop"), initFn("w25_kmod_stop_soft")]).join("\n");
  return {dir: dir, run: run, module: module, hd: hd, hold: hold, stop: stop};
}
function waitFor(cond, ms) {
  var end = Date.now() + ms;
  while (Date.now() < end) { if (cond()) return true; spawnSync("sleep", ["0.1"]); }
  return cond();
}
function loaded(h) { return /PASSTHROUGH ACTIVE/.test(read(h.run + "/hdmi-watch.out") || ""); }

test("stopping the holder unloads the module and records its exit; 20 asks for a restart", function () {
  [false, true].forEach(function (fails) {
    var h = holder(fails);
    try {
      var child = require("node:child_process").spawn("sh", [h.hold], {stdio: "ignore"});
      assert.ok(waitFor(function () { return loaded(h); }, 5000));
      var r = sh(h.stop + "\nw25_kmod_stop; echo \"status=$?\"");
      assert.match(r.stdout, /status=0/);
      assert.strictEqual(/WARN_KMOD=.*Restart the TV/.test(r.stdout), fails, r.stdout);
      assert.strictEqual(read(h.run + "/hdmi-watch.exit"), (fails ? 20 : 0) + "\n");
      assert.strictEqual(fs.existsSync(h.run + "/hdmi.pid"), false);
      child.kill();
      assert.strictEqual(sh(h.stop + "\nw25_kmod_stop_soft").stdout, "", "a second stop is a no-op");
    } finally {
      fs.rmSync(h.dir, {recursive: true, force: true});
    }
  });
});

test("the module is wanted only with DTS-HD on, and the holder unloads it once DTS-HD is off", function () {
  var h = holder(false);
  try {
    var child = require("node:child_process").spawn("sh", [h.hold], {stdio: "ignore"});
    assert.ok(waitFor(function () { return loaded(h); }, 5000));
    fs.unlinkSync(h.hd);
    assert.ok(waitFor(function () { return read(h.run + "/hdmi-watch.exit") === "0\n"; }, 5000));
    assert.strictEqual(fs.existsSync(h.module), false);
    child.kill();
  } finally {
    fs.rmSync(h.dir, {recursive: true, force: true});
  }
});

test("a load the TV did not survive is not retried at boot; turning DTS-HD on clears it", function () {
  var dir = tmp("dts-kmod-crash-");
  try {
    var kmod = path.join(dir, "hdmi");
    ["earc_route-5.4.ko", "earc-route-load.sh"].forEach(function (f) { write(path.join(kmod, f), "x"); });
    write(path.join(kmod, "loading"), "");
    var r = sh(['uname() { echo 5.4.268-329.ptl4tv.5; }', "KMOD_DIR=" + kmod, initFn("w25_kmod_refusal"), "w25_kmod_refusal"].join("\n"));
    assert.strictEqual(r.stdout, "crashed\n");
    assert.match(service.w25KmodReason("crashed"), /restarted while the DTS-HD kernel module was loading/);
    var on = service.w25PassthroughSteps(true, true);
    assert.ok(on.indexOf('rm -f "$KMOD_DIR/loading"') < on.indexOf("R=$(w25_kmod_refusal)"), "the user's retry clears the marker first");
    assert.doesNotMatch(service.w25PassthroughSteps(true), /loading/, "only DTS-HD on retries");
    var start = initFn("w25_kmod_start");
    assert.ok(start.includes('[ "$WHY" != crashed ] || w25_kmod_crash_alert'), "boot tells the user once it refuses after a crash");
    var alert = initFn("w25_kmod_crash_alert");
    assert.match(alert, /createAlert/);
    assert.match(alert, /"onclick":"luna:\/\/com\.webos\.applicationManager\/launch","params":\{"id":"io\.github\.josippapez\.dtsenabler"\}/);
    assert.match(alert, /\) <\/dev\/null >\/dev\/null 2>&1 &$/m, "detached, so the boot hook does not wait for it");
    JSON.parse(/'(\{"sourceId".*\})'/.exec(alert)[1]);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("the holder marks only the load itself, so a normal power-off later does not count as a crash", function () {
  var h = holder(false);
  try {
    var mark = path.join(h.dir, "hdmi", "loading");
    var child = require("node:child_process").spawn("sh", [h.hold], {stdio: "ignore"});
    assert.ok(waitFor(function () { return loaded(h) && fs.existsSync(h.run + "/hdmi.pid"); }, 5000));
    assert.ok(fs.existsSync(path.join(h.dir, "marked-during-load")), "marked while the loader runs");
    assert.ok(waitFor(function () { return !fs.existsSync(mark); }, 1000), "cleared as soon as the load returns");
    fs.unlinkSync(h.hd);
    assert.ok(waitFor(function () { return read(h.run + "/hdmi-watch.exit") === "0\n"; }, 5000));
    child.kill();
    var f = holder(false, true);
    spawnSync("sh", [f.hold], {encoding: "utf8", timeout: 10000});
    assert.ok(fs.existsSync(path.join(f.dir, "marked-during-load")));
    assert.strictEqual(fs.existsSync(path.join(f.dir, "hdmi", "loading")), false, "a refused load is not a crash");
    fs.rmSync(f.dir, {recursive: true, force: true});
  } finally {
    fs.rmSync(h.dir, {recursive: true, force: true});
  }
});

test("the holder ends when the module does not load, and a second one does not start beside it", function () {
  var h = holder(false, true);
  try {
    var r = spawnSync("sh", [h.hold], {encoding: "utf8", timeout: 10000});
    assert.strictEqual(r.status, 0, r.stderr);
    assert.strictEqual(read(h.run + "/hdmi-watch.exit"), "1\n");
    write(h.run + "/hdmi.pid", process.pid + "\n");
    fs.rmSync(h.run + "/hdmi-watch.out");
    spawnSync("sh", [h.hold], {encoding: "utf8", timeout: 10000});
    assert.strictEqual(fs.existsSync(h.run + "/hdmi-watch.out"), false, "no second load");
  } finally {
    fs.rmSync(h.dir, {recursive: true, force: true});
  }
});

test("passthrough starts and stops the module; boot, stand-down, self-heal, Disable and Uninstall do too", function () {
  var mode = body.indexOf('if [ "${W25_KMOD:-}" = run ]; then');
  assert.ok(mode !== -1 && mode < body.indexOf("# --- G) self-heal"), "the holder's mode exits before the apply path");
  assert.ok(body.includes("start) w25_pt_start; w25_kmod_start ;;"));
  assert.ok(body.includes("stop) w25_pt_stop || exit 1; w25_kmod_stop || exit 1 ;;"));
  assert.ok(body.includes("\nw25_pt_start\nw25_kmod_start\n"), "the boot apply starts it");
  assert.ok(body.indexOf("\nw25_kmod_start\n") > body.indexOf("# --- GATE 2a) loader resolution"), "only after the gates");
  assert.ok(initFn("w25_kmod_start").includes("W25_PT= W25_APPDTS= W25_KMOD=run setsid"),
    "the holder does not inherit the mode that launched it");
  assert.ok(initFn("w25_stand_down").includes("w25_kmod_stop_soft"));
  assert.ok(initFn("w25_self_heal").includes("w25_kmod_stop_soft"));
  var disable = service.w25Disable();
  var stop = disable.indexOf("\nw25_kmod_stop_soft\n");
  assert.ok(stop >= 0 && stop < disable.indexOf('rm -f "/var/lib/webosbrew/init.d/restore_dts25"'), "Disable stops it");
  var uninstall = service.w25Uninstall();
  assert.ok(uninstall.indexOf("\nw25_kmod_stop_soft\n") < uninstall.indexOf('rm -rf "'), "Uninstall stops it before deleting");
  assert.strictEqual(/W25_HDMI|HDMI_FLAG|hdmi-passthrough/.test(body), false, "the HDMI opt-in is gone");
});

test("install.sh and Enable stage the module and its loader", function () {
  var dir = tmp("dts-kmod-stage-");
  try {
    var files = ["earc_route-5.4.ko", "earc_route-6.12.ko", "earc-route-load.sh"];
    var src = path.join(dir, "src");
    files.forEach(function (f) { write(path.join(src, "kmod-out", f), f); });
    var install = fs.readFileSync(path.join(ROOT, "webos25/restore/install.sh"), "utf8");
    var block = install.slice(install.indexOf("\nKMOD_DEST="), install.indexOf("\n# --- 2. Stage TrueHD payload"));
    var dest = path.join(dir, "dts25");
    var replace = 'replace_file() { cp -Pf "$1" "$2.new" && mv -f "$2.new" "$2" && return 0; rm -f "$2.new"; return 1; }';
    ok(sh(["log() { :; }", replace, "SELF_DIR=" + src, "DTS_DEST=" + dest, block].join("\n")), "install.sh step 1d");
    files.forEach(function (f) { assert.strictEqual(read(path.join(dest, "hdmi", f)), f); });
    assert.ok(service.w25Enable(false).includes("/payload/webos25-hdmi/$f"), "Enable stages from the app payload");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("the read-only probe reports the module", function () {
  var r = sh(service.DETECT_PROBE);
  assert.strictEqual(r.status, 0, r.stderr);
  ["KMOD_KERNEL=", "KMOD_REFUSAL=kernel", "KMOD_LOADED=0", "KMOD_LOADER="].forEach(function (k) {
    assert.match(r.stdout, new RegExp("^" + k, "m"), k);
  });
});

function mockedService(probes, stdout) {
  var calls = [];
  var m = loadService({
    detectProfile: function () { return Promise.resolve({profile: "webos25-armel-gst124", probes: probes || {}}); },
    rootExec: function (cmd) { calls.push(cmd); return Promise.resolve({stdout: stdout, stderr: ""}); },
    logActionResult: function () {}
  });
  m.calls = calls;
  return m;
}
function call(m, method, payload) {
  return new Promise(function (resolve) { m.handlers[method]({payload: payload || {}, respond: resolve}); });
}

test("turning DTS-HD on refuses where the module cannot run; status says whether it is loaded", function () {
  var steps = service.w25PassthroughSteps(true, true);
  assert.strictEqual(spawnSync("sh", ["-n"], {input: steps, encoding: "utf8"}).status, 0);
  assert.match(steps, /R=\$\(w25_kmod_refusal\)/);
  assert.doesNotMatch(service.w25PassthroughSteps(true), /w25_kmod_refusal\)/, "core passthrough needs no module");
  return Promise.all([
    call(mockedService({}, "KMOD_REFUSED=kernel\nKMOD_KERNEL=6.6.1\n"), "setPassthrough", {enabled: true, hd: true}).then(function (res) {
      assert.strictEqual(res.returnValue, false);
      assert.match(res.errorText, /DTS-HD needs a supported kernel; this TV has 6\.6\.1/);
    }),
    call(mockedService({KMOD_KERNEL: "5.4.268-329.ptl4tv.5", KMOD_REFUSAL: "", KMOD_LOADED: "1"}, "HD_FLAG=1\n"), "status").then(function (s) {
      assert.strictEqual(s.kmodLoaded, true);
      assert.strictEqual(s.kmodRefusal, "");
    }),
    call(mockedService({KMOD_KERNEL: "6.6.1", KMOD_REFUSAL: "kernel", KMOD_LOADED: "0"}, ""), "status").then(function (s) {
      assert.match(s.kmodRefusal, /supported kernel/);
    }),
    call(mockedService({}, "WARN_KMOD=The DTS-HD kernel module could not undo all of its changes. Restart the TV.\nREG_REVERTED=1\nOK\n"), "disable").then(function (res) {
      assert.match(res.warning, /could not undo all of its changes/);
    })
  ]);
});
