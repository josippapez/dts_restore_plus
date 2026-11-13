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

test("the module runs on verified 5.4 kernels and refuses unverified 6.12", function () {
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
    assert.strictEqual(refusal("6.12.44-352"), "[kernel-6.12]");
    assert.strictEqual(refusal("6.6.1"), "[kernel]");
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.9"), "[kernel]");
    fs.unlinkSync(path.join(kmod, "earc_route-6.12.ko"));
    assert.strictEqual(refusal("6.12.44-352"), "[kernel-6.12]");
    write(path.join(gh, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[glasshouse]");
    write(path.join(core, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[lg-dts-core]");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

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
  var vars = ["KMOD_DIR=" + kmod, "KMOD_OUT=" + run + "/hdmi-watch.out",
    "KMOD_MODULE=" + module, "PT_FLAG=" + pt, "HD_FLAG=" + hd,
    "KMOD_UNIT=dts25-test.service", 'systemctl() { :; }',
    oneLine("w25_module_wanted"), "w25_log() { :; }",
    rmmodFails ? "rmmod() { echo 'rmmod: busy' >&2; return 1; }" : 'rmmod() { rm -rf "' + module + '"; }'];
  var hold = path.join(dir, "hold.sh");
  write(hold, vars.concat([initFn("w25_kmod_hold"), "w25_kmod_hold"]).join("\n"));
  var stop = vars.concat([initFn("w25_kmod_stop"), initFn("w25_kmod_stop_soft")]).join("\n");
  return {dir: dir, run: run, module: module, hd: hd, hold: hold, stop: stop};
}
function loaded(h) { return /PASSTHROUGH ACTIVE/.test(read(h.run + "/hdmi-watch.out") || ""); }

test("stop verifies the module is gone, with or without its unit", function () {
  [false, true].forEach(function (fails) {
    var h = holder(fails);
    try {
      ok(sh(fs.readFileSync(h.hold, "utf8")), "module load");
      assert.ok(loaded(h));
      var r = sh(h.stop + "\nw25_kmod_stop; echo \"status=$?\"");
      assert.match(r.stdout, fails ? /ERROR=.*still loaded[\s\S]*status=1/ : /status=0/);
      assert.strictEqual(fs.existsSync(h.module), fails);
      assert.strictEqual(/WARN_KMOD=.*still loaded/.test(sh(h.stop + "\nw25_kmod_stop_soft").stdout), fails);
    } finally {
      fs.rmSync(h.dir, {recursive: true, force: true});
    }
  });
});

test("the module is wanted only with passthrough and DTS-HD on", function () {
  var h = holder(false);
  try {
    assert.strictEqual(sh(h.stop + "\nw25_module_wanted").status, 0);
    fs.unlinkSync(h.hd);
    assert.strictEqual(sh(h.stop + "\nw25_module_wanted").status, 1);
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
    // Boot refuses it with a TV toast once boot is done, detached from the hook.
    var calls = path.join(dir, "calls");
    exe(path.join(dir, "bin", "systemd-run"), ': > "' + path.join(dir, "started") + '"');
    var r2 = sh(['uname() { echo 5.4.268-329.ptl4tv.5; }', "KMOD_DIR=" + kmod,
      "PT_FLAG=" + path.join(dir, "pt"), "HD_FLAG=" + path.join(dir, "pt"), ': > "$PT_FLAG"',
      "w25_log() { :; }", 'systemctl() { return 1; }', 'PATH="' + path.join(dir, "bin") + ':$PATH"',
      'toast() { echo "$1" > ' + calls + '; }', oneLine("w25_module_wanted"),
      initFn("w25_kmod_refusal"), "w25_wait_boot() { :; }", initFn("w25_kmod_start"), "w25_kmod_start; wait"].join("\n"));
    assert.strictEqual(r2.status, 0, r2.stderr);
    assert.strictEqual(fs.existsSync(path.join(dir, "started")), false, "a crashed load is not started again");
    assert.match(read(calls) || "", /^DTS Enabler: DTS-HD is paused.*turn Full DTS-HD off and on/);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("the oneshot marks only the load itself, not later power-off or a refused load", function () {
  var h = holder(false);
  try {
    var mark = path.join(h.dir, "hdmi", "loading");
    ok(sh(fs.readFileSync(h.hold, "utf8")), "module load");
    assert.ok(loaded(h));
    assert.ok(fs.existsSync(path.join(h.dir, "marked-during-load")), "marked while the loader runs");
    assert.strictEqual(fs.existsSync(mark), false, "cleared as soon as the load returns");
    var f = holder(false, true);
    spawnSync("sh", [f.hold], {encoding: "utf8", timeout: 10000});
    assert.ok(fs.existsSync(path.join(f.dir, "marked-during-load")));
    assert.strictEqual(fs.existsSync(path.join(f.dir, "hdmi", "loading")), false, "a refused load is not a crash");
    fs.rmSync(f.dir, {recursive: true, force: true});
  } finally {
    fs.rmSync(h.dir, {recursive: true, force: true});
  }
});

test("the oneshot fails when the loader refuses", function () {
  var h = holder(false, true);
  try {
    var r = spawnSync("sh", [h.hold], {encoding: "utf8", timeout: 10000});
    assert.strictEqual(r.status, 1, r.stderr);
  } finally {
    fs.rmSync(h.dir, {recursive: true, force: true});
  }
});

test("passthrough starts and stops the module; boot, stand-down, self-heal, Disable and Uninstall do too", function () {
  var mode = body.indexOf('if [ "${W25_KMOD:-}" = run ]; then');
  assert.ok(mode !== -1 && mode < body.indexOf("# --- G) self-heal"), "the holder's mode exits before the apply path");
  assert.ok(body.includes("start) w25_pt_start; w25_kmod_start ;;"));
  assert.ok(body.includes("stop) w25_pt_stop || exit 1; w25_kmod_stop || exit 1 ;;"));
  assert.ok(body.includes("\nw25_pt_start\nw25_kmod_start --no-block\n"), "boot only enqueues the load");
  assert.ok(body.indexOf("\nw25_kmod_start --no-block\n") > body.indexOf("# --- GATE 2a) loader resolution"), "only after the gates");
  var start = initFn("w25_kmod_start");
  ["systemd-run", "--collect", "Type=oneshot", "RemainAfterExit=yes", "ExecStop=", "W25_KMOD=run"].forEach(function (s) {
    assert.ok(start.includes(s), s);
  });
  // The boot script runs under set -u; the toggle calls this with no argument.
  var dir = tmp("dts-kmod-run-"), args = path.join(dir, "args");
  try {
    exe(path.join(dir, "bin", "systemd-run"), 'echo "$*" > "' + args + '"');
    var kmod = path.join(dir, "hdmi");
    ["earc_route-5.4.ko", "earc-route-load.sh"].forEach(function (f) { write(path.join(kmod, f), "x"); });
    ok(sh(["set -u", 'PATH="' + path.join(dir, "bin") + ':$PATH"', 'uname() { echo 5.4.268-329.ptl4tv.5; }',
      "KMOD_DIR=" + kmod, "KMOD_OUT=" + path.join(dir, "out"), "KMOD_UNIT=dts25-earc.service", "INIT_SELF=/x",
      "PT_FLAG=" + args, "HD_FLAG=" + args, ': > "$PT_FLAG"', "w25_log() { :; }", "systemctl() { return 1; }",
      oneLine("w25_module_wanted"), initFn("w25_kmod_refusal"), start.split("/run/dts25").join(dir), "w25_kmod_start"].join("\n")), "no-argument start");
    assert.match(read(args), /--collect -u dts25-earc\.service/);
    assert.doesNotMatch(read(args), /--no-block/);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
  assert.doesNotMatch(service.w25PassthroughSteps(true, true), /hdmi-watch\.exit|\/dev\/null 2>&1 &/, "the toggle waits for the start job");
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
    }),
    call(mockedService({}, "WARN_KMOD=The DTS-HD kernel module is still loaded. Restart the TV.\nOK\n"), "setPassthrough", {enabled: false}).then(function (res) {
      assert.strictEqual(res.enabled, false);
      assert.match(res.warning, /still loaded/);
      assert.strictEqual(res.summary, res.warning);
    })
  ]);
});
