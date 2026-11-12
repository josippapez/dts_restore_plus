"use strict";

/* DTS-45: HDMI-input DTS passthrough (webos25/kmod earc_route) as an app opt-in. */
var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, sh, write, exe, ok, initFn, loadService, ROOT} = require("./helpers");

var body = service.w25InitScriptBody();
var lines = body.split("\n");
function oneLine(name) {
  var l = lines.filter(function (x) { return x.indexOf(name + "() { ") === 0; });
  assert.strictEqual(l.length, 1, name);
  return l[0];
}
function tmp(prefix) { return fs.mkdtempSync(path.join(os.tmpdir(), prefix)); }
function read(file) { return fs.existsSync(file) ? fs.readFileSync(file, "utf8") : null; }

test("the opt-in runs only on the two 5.4.268 kernels, beside no enabled DTS tool, with its payload", function () {
  var dir = tmp("dts-hdmi-refusal-");
  try {
    var hdmi = path.join(dir, "hdmi"), core = path.join(dir, "lg-dts-core"), gh = path.join(dir, "lg-dts-native");
    ["earc_route-5.4.ko", "earc-route-load.sh"].forEach(function (f) { write(path.join(hdmi, f), "x"); });
    var fn = initFn("w25_hdmi_refusal").split("/var/lib/lg-dts-core").join(core).split("/var/lib/lg-dts-native").join(gh);
    function refusal(kernel) {
      var r = sh(['uname() { echo "' + kernel + '"; }', "HDMI_DIR=" + hdmi, fn, 'printf "[%s]" "$(w25_hdmi_refusal)"'].join("\n"));
      assert.strictEqual(r.status, 0, r.stderr);
      return r.stdout;
    }
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[]");
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.2"), "[]");
    assert.strictEqual(refusal("6.12.44-352"), "[kernel-6.12]", "the 6.12 module has never run on a TV");
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.9"), "[kernel]");
    // Installed but not enabled: allowed (the app warns).
    fs.mkdirSync(core);
    fs.mkdirSync(gh);
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[]");
    write(path.join(gh, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[glasshouse]");
    write(path.join(core, "enabled"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[lg-dts-core]");
    fs.rmSync(core, {recursive: true});
    fs.rmSync(gh, {recursive: true});
    fs.unlinkSync(path.join(hdmi, "earc-route-load.sh"));
    assert.strictEqual(refusal("5.4.268-329.ptl4tv.5"), "[payload]");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("a process counts as alive until it is a zombie", function () {
  var dir = tmp("dts-hdmi-alive-");
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

/* A shell holding the real EDID functions, with eim, configd and the sound output
 * faked: eim loads the hook only when its env file names it (as on a C5). */
function edidShell(opts) {
  var dir = tmp("dts-hdmi-edid-");
  var f = {
    dir: dir, calls: path.join(dir, "calls"), eimEnv: path.join(dir, "eim.env"), hooked: path.join(dir, "hooked"),
    sndout: path.join(dir, "sndout"), output: path.join(dir, "output"), clock: path.join(dir, "clock"),
    appdts: path.join(dir, "appdts.enabled"), lls: path.join(dir, "factorydb.json"), myEdid: path.join(dir, "libedidaudio.so")
  };
  write(f.lls, '{"tv.model":{"edidType":"' + (opts && opts.stock || "TrueHD") + '"}}');
  write(f.myEdid, "");
  write(path.join(dir, "eim.service"), "[Service]\nEnvironment=LD_PRELOAD=$LD_PRELOAD:/usr/lib/libsystrim.so.3\n");
  write(f.clock, "1000");
  f.set = function (receiver, output) {
    write(f.sndout, "[EARC]\nEArcOnOff=" + (receiver ? "On" : "Off") + "\n");
    write(f.output, output || (receiver ? "external_arc" : "tv_speaker"));
  };
  f.hook = function (on) {
    if (on) { write(f.eimEnv, "LD_PRELOAD=" + f.myEdid + "\n"); write(f.hooked, ""); }
    else { fs.rmSync(f.eimEnv, {force: true}); fs.rmSync(f.hooked, {force: true}); }
  };
  f.log = function () { return (read(f.calls) || "").split("\n").filter(Boolean); };
  f.preamble = [
    "set -u", "LOG=/dev/null",
    "EIM_ENV=" + f.eimEnv, "EIM_UNIT=" + path.join(dir, "eim.service"), "MYEDID=" + f.myEdid,
    "EDID_STATE=" + path.join(dir, "edid-state"), "APPDTS_FLAG=" + f.appdts, "LLS=" + f.lls,
    "HDMI_SETTLE=3", "HDMI_COOLDOWN=300",
    "w25_log() { :; }", "sleep() { :; }",
    "date() { cat " + f.clock + "; }",
    "w25_eim_hooked() { [ -f " + f.hooked + " ]; }",
    "systemctl() {",
    "  echo \"systemctl $*\" >> " + f.calls,
    "  if grep -q libedidaudio \"$EIM_ENV\" 2>/dev/null; then : > " + f.hooked + "; echo remap > \"$EDID_STATE\"; else rm -f " + f.hooked + "; fi",
    "}",
    "script() {",
    "  echo luna >> " + f.calls,
    "  case \"$4\" in *getSystemSettings*'\"soundOutput\"'*)",
    "    printf '{\"subscribed\":false,\"category\":\"sound\",\"method\":\"getSystemSettings\",\"settings\":{\"soundOutput\":\"%s\"},\"returnValue\":true}\\r\\n' \"$(cat " + f.output + ")\" ;;",
    "  esac",
    "}",
    oneLine("w25_luna"), oneLine("w25_edid_stock"),
    initFn("w25_hdmi_hook_on"), initFn("w25_hdmi_hook_off"),
    initFn("w25_hdmi_receiver").split("/proc/lgtv-driver/audio/sndout").join(f.sndout),
    initFn("w25_hdmi_edid"), initFn("w25_hdmi_poll")
  ].join("\n");
  // Runs N polls in one shell, like the supervisor's loop, and returns its state.
  f.polls = function (n, state) {
    var r = sh([f.preamble, state || "want= same=0 changed=0",
      "i=0; while [ \"$i\" -lt " + n + " ]; do w25_hdmi_poll; i=$((i + 1)); done",
      'echo "want=$want same=$same changed=$changed"'].join("\n"));
    assert.strictEqual(r.status, 0, r.stderr);
    return r.stdout.trim();
  };
  return f;
}

test("HDMI inputs are offered DTS only once an eARC receiver has held as the output", function () {
  var f = edidShell();
  try {
    write(f.appdts, "");
    f.hook(true);
    // TV speakers: nothing changes, and luna is not even asked.
    f.set(false);
    var state = f.polls(5);
    assert.deepStrictEqual(f.log(), []);
    assert.ok(fs.existsSync(f.eimEnv));
    // An eARC receiver: two polls are not enough, the third takes the hook off once.
    f.set(true);
    state = f.polls(2, state);
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), []);
    state = f.polls(3, state);
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), ["systemctl restart eim"]);
    assert.strictEqual(fs.existsSync(f.eimEnv), false, "the hook is off: the EDIDs carry DTS");
    assert.match(state, /^want=dts same=5 changed=1000$/);
    // Back to the speakers: the hook goes back on after the output held.
    fs.writeFileSync(f.calls, "");
    f.set(false);
    write(f.clock, "1060");
    state = f.polls(3, state);
    assert.deepStrictEqual(f.log(), ["systemctl restart eim"]);
    assert.ok(fs.existsSync(f.eimEnv), "no DTS in the HDMI EDIDs on the speakers");
    // The receiver again within the cooldown: DTS waits, so eim is not restarted again.
    fs.writeFileSync(f.calls, "");
    f.set(true);
    write(f.clock, "1200");
    state = f.polls(4, state);
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), []);
    write(f.clock, "1360");
    state = f.polls(1, state);
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), ["systemctl restart eim"]);
    assert.strictEqual(fs.existsSync(f.eimEnv), false);
  } finally {
    fs.rmSync(f.dir, {recursive: true, force: true});
  }
});

test("eARC on with the TV speakers selected is not a receiver", function () {
  var f = edidShell();
  try {
    write(f.appdts, "");
    f.hook(true);
    f.set(true, "tv_speaker");
    assert.strictEqual(f.polls(5), "want=nodts same=5 changed=0");
    assert.ok(f.log().includes("luna"), "the sound output was asked");
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), []);
    assert.ok(fs.existsSync(f.eimEnv));
  } finally {
    fs.rmSync(f.dir, {recursive: true, force: true});
  }
});

test("the EDID is left alone where LG lists DTS itself or Show DTS tracks in apps is off", function () {
  var f = edidShell({stock: "TrueHD+dts"});
  try {
    write(f.appdts, "");
    f.hook(true);
    f.set(true);
    assert.strictEqual(f.polls(5), "want=dts same=5 changed=0");
    f.set(false);
    assert.strictEqual(f.polls(5), "want=nodts same=5 changed=0");
    assert.deepStrictEqual(f.log().filter(function (l) { return l !== "luna"; }), []);
  } finally {
    fs.rmSync(f.dir, {recursive: true, force: true});
  }
  f = edidShell();
  try {
    f.set(false);
    assert.strictEqual(f.polls(5), "want=nodts same=5 changed=0");
    assert.deepStrictEqual(f.log(), [], "no app-DTS marker: no hook to put back");
    assert.strictEqual(fs.existsSync(f.eimEnv), false);
  } finally {
    fs.rmSync(f.dir, {recursive: true, force: true});
  }
});

/* The supervisor and its stop, with a fake loader that "loads" the module by
 * creating its sysfs directory, and an rmmod that removes it, or fails (exit 20). */
function supervisorRun(exitCode, loadFails) {
  var f = edidShell();
  var hdmi = path.join(f.dir, "hdmi"), run = path.join(f.dir, "run"), module = path.join(f.dir, "module");
  fs.mkdirSync(run);
  exe(path.join(hdmi, "earc-route-load.sh"), loadFails
    ? 'echo "earc-route: REFUSED: test"; exit 1'
    : 'mkdir -p "' + module + '"; echo "earc-route: PASSTHROUGH ACTIVE (mode=7) EXPERIMENTAL"');
  write(f.appdts, "");
  f.hook(false);  // a receiver: the hook stays off until the supervisor stops
  f.set(true);
  var vars = ["HDMI_DIR=" + hdmi, "HDMI_PID=" + run + "/hdmi.pid", "HDMI_OUT=" + run + "/hdmi-watch.out",
    "HDMI_EXIT=" + run + "/hdmi-watch.exit", "HDMI_POLL=0.1", "HDMI_MODULE=" + module,
    exitCode ? "rmmod() { echo 'rmmod: busy' >&2; return 1; }" : 'rmmod() { rm -rf "' + module + '"; }',
    // macOS has no /proc: the same zombie rule, from ps.
    'w25_alive() { [ -n "$1" ] || return 1; s=$(ps -o stat= -p "$1" 2>/dev/null) || return 1; case $s in Z*|"") return 1 ;; esac; }'];
  var sup = path.join(f.dir, "supervise.sh");
  write(sup, [f.preamble.replace("sleep() { :; }", "")].concat(vars, [initFn("w25_hdmi_supervise"), "w25_hdmi_supervise"]).join("\n"));
  var stop = [vars.join("\n"), "w25_log() { :; }", initFn("w25_hdmi_stop"), initFn("w25_hdmi_stop_soft")].join("\n");
  return {f: f, run: run, sup: sup, stop: stop};
}
function waitFor(cond, ms) {
  var end = Date.now() + ms;
  while (Date.now() < end) { if (cond()) return true; spawnSync("sleep", ["0.1"]); }
  return cond();
}

test("stopping the supervisor unloads the module, records its exit and puts the EDID hook back", function () {
  [0, 20].forEach(function (code) {
    var s = supervisorRun(code);
    try {
      var child = require("node:child_process").spawn("sh", [s.sup], {stdio: "ignore"});
      assert.ok(waitFor(function () { return /PASSTHROUGH ACTIVE/.test(read(s.run + "/hdmi-watch.out") || ""); }, 5000));
      assert.match(read(s.run + "/hdmi.pid"), /^\d+\n$/);
      var r = sh(s.stop + "\nw25_hdmi_stop; echo \"status=$?\"");
      assert.strictEqual(r.status, 0, r.stderr);
      assert.match(r.stdout, /status=0/);
      assert.strictEqual(/WARN_HDMI=.*Restart the TV/.test(r.stdout), code === 20, r.stdout);
      assert.strictEqual(read(s.run + "/hdmi-watch.exit"), code + "\n");
      assert.strictEqual(fs.existsSync(s.run + "/hdmi.pid"), false, "the supervisor is gone");
      assert.ok(fs.existsSync(s.f.eimEnv), "the app-DTS hook is back on");
      assert.deepStrictEqual(s.f.log().filter(function (l) { return l !== "luna"; }), ["systemctl restart eim"]);
      child.kill();
      // Nothing left to stop: a second stop is a no-op.
      assert.strictEqual(sh(s.stop + "\nw25_hdmi_stop_soft").stdout, "");
    } finally {
      fs.rmSync(s.f.dir, {recursive: true, force: true});
    }
  });
});

test("the supervisor ends when the module does not load and a second one does not start beside it", function () {
  var s = supervisorRun(0, true);
  try {
    var r = spawnSync("sh", [s.sup], {encoding: "utf8", timeout: 10000});
    assert.strictEqual(r.status, 0, r.stderr);
    assert.strictEqual(read(s.run + "/hdmi-watch.exit"), "1\n");
    assert.strictEqual(fs.existsSync(s.run + "/hdmi.pid"), false);
    // A live pid file means a supervisor already runs: the new one leaves at once.
    write(s.run + "/hdmi.pid", process.pid + "\n");
    fs.rmSync(s.run + "/hdmi-watch.out");
    r = spawnSync("sh", [s.sup], {encoding: "utf8", timeout: 10000});
    assert.strictEqual(r.status, 0);
    assert.strictEqual(fs.existsSync(s.run + "/hdmi-watch.out"), false, "no second load");
    assert.strictEqual(read(s.run + "/hdmi.pid"), process.pid + "\n");
  } finally {
    fs.rmSync(s.f.dir, {recursive: true, force: true});
  }
});

test("Turn on refuses without the app-DTS edidType or on a refused TV, and otherwise starts", function () {
  var dir = tmp("dts-hdmi-on-");
  try {
    var flag = path.join(dir, "hdmi-passthrough"), started = path.join(dir, "started");
    function on(refusal, stock, live) {
      fs.rmSync(flag, {force: true});
      fs.rmSync(started, {force: true});
      var r = sh([
        "HDMI_FLAG=" + flag, "HDMI_OUT=" + path.join(dir, "out"), "HDMI_EXIT=" + path.join(dir, "exit"),
        'w25_hdmi_refusal() { echo "' + refusal + '"; }',
        'w25_edid_stock() { echo "' + stock + '"; }', 'w25_edid_get() { echo "' + live + '"; }',
        'w25_hdmi_start() { : > ' + started + '; echo 1 > "$HDMI_EXIT"; }', "sleep() { :; }",
        initFn("w25_hdmi_on"), "w25_hdmi_on; echo \"status=$?\""].join("\n"));
      assert.strictEqual(r.status, 0, r.stderr);
      return r.stdout;
    }
    assert.strictEqual(on("", "TrueHD", "TrueHD"), "HDMI_REFUSED=appdts\nstatus=0\n");
    assert.strictEqual(fs.existsSync(flag), false);
    assert.strictEqual(on("lg-dts-core", "TrueHD", "TrueHD+dts"), "HDMI_REFUSED=lg-dts-core\nstatus=0\n");
    assert.strictEqual(fs.existsSync(started), false);
    assert.strictEqual(on("", "TrueHD", "TrueHD+dts"), "status=0\n");
    assert.ok(fs.existsSync(flag) && fs.existsSync(started));
    // A TV whose factory value lists dts offers it on HDMI by itself.
    assert.strictEqual(on("", "TrueHD+dts", "TrueHD+dts"), "status=0\n");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("boot starts the opt-in after the gates; every way out stops it", function () {
  var mode = body.indexOf('if [ -n "${W25_HDMI:-}" ]; then');
  assert.ok(mode !== -1 && mode < body.indexOf("# --- G) self-heal"), "W25_HDMI mode exits before the apply path");
  assert.ok(body.includes("\nw25_pt_start\nw25_hdmi_start\n"), "the boot apply starts it");
  assert.ok(body.indexOf("\nw25_hdmi_start\n") > body.indexOf("# --- GATE 2a) loader resolution"), "only after the gates");
  assert.ok(initFn("w25_stand_down").includes("w25_hdmi_stop_soft"), "stand-down stops it");
  assert.ok(initFn("w25_self_heal").includes("w25_hdmi_stop_soft"), "self-heal stops it before deleting dts25");
  assert.ok(body.includes("off) w25_hdmi_stop || exit 1; rm -f \"$HDMI_FLAG\" ;;"), "the toggle keeps the marker when it cannot stop");
  var disable = service.w25Disable();
  var stop = disable.indexOf("\nw25_hdmi_stop_soft\n");
  assert.ok(stop >= 0 && stop < disable.indexOf('rm -f "/var/lib/webosbrew/init.d/restore_dts25"'), "Disable stops it");
  var uninstall = service.w25Uninstall();
  assert.ok(uninstall.indexOf("\nw25_hdmi_stop_soft\n") < uninstall.indexOf('rm -rf "'), "Uninstall stops it before deleting");
  // The supervisor's functions exist before the mode runs them.
  ["w25_hdmi_supervise() {", "w25_hdmi_on() {", "w25_hdmi_hook_on() {", "w25_luna() {"].forEach(function (fn) {
    assert.ok(body.indexOf(fn) !== -1 && body.indexOf(fn) < mode, fn);
  });
});

test("CLI uninstall turns HDMI passthrough off only with a script that knows W25_HDMI", function () {
  var source = fs.readFileSync(path.join(ROOT, "webos25/restore/uninstall.sh"), "utf8");
  var start = source.indexOf("\nSTOP_SCRIPT=");
  var end = source.indexOf("\n# --- 1. Remove boot hook");
  var block = source.slice(start, end);
  [[50, false], [51, true]].forEach(function (c) {
    var dir = tmp("dts-hdmi-uninstall-");
    try {
      var called = path.join(dir, "called");
      exe(path.join(dir, "init_dts25.sh"), "W25_GATE_VERSION=" + c[0] + '\n[ -z "${W25_HDMI:-}" ] || printf "%s" "$W25_HDMI" > "$CALLED"\nexit ' + (c[1] ? 1 : 0));
      var r = spawnSync("sh", ["-c", 'log() { echo "$*"; }' + block, path.join(dir, "uninstall.sh")],
        {encoding: "utf8", env: Object.assign({}, process.env, {DTS_DEST: path.join(dir, "x"), CALLED: called})});
      assert.strictEqual(r.status, 0, r.stderr);
      assert.strictEqual(read(called), c[1] ? "off" : null);
      if (c[1]) assert.match(r.stdout, /WARN HDMI passthrough stop failed; continuing/);
    } finally {
      fs.rmSync(dir, {recursive: true, force: true});
    }
  });
});

test("install.sh and Enable stage the module and its loader", function () {
  var dir = tmp("dts-hdmi-stage-");
  try {
    var files = ["earc_route-5.4.ko", "earc-route-load.sh"];
    var src = path.join(dir, "src");
    files.forEach(function (f) { write(path.join(src, "kmod-out", f), f); });
    var install = fs.readFileSync(path.join(ROOT, "webos25/restore/install.sh"), "utf8");
    var block = install.slice(install.indexOf("\nHDMI_DEST="), install.indexOf("\n# --- 2. Stage TrueHD payload"));
    var dest = path.join(dir, "dts25");
    var replace = 'replace_file() { cp -Pf "$1" "$2.new" && mv -f "$2.new" "$2" && return 0; rm -f "$2.new"; return 1; }';
    ok(sh(["log() { :; }", replace, "SELF_DIR=" + src, "DTS_DEST=" + dest, block].join("\n")), "install.sh step 1d");
    files.forEach(function (f) { assert.strictEqual(read(path.join(dest, "hdmi", f)), f); });
    var enable = service.w25Enable(false);
    assert.ok(enable.includes("/payload/webos25-hdmi/$f"), "Enable stages from the app payload");
    var step = enable.slice(enable.indexOf('mkdir -p "/var/lib/webosbrew/dts25/hdmi"'));
    step = step.slice(0, step.indexOf("\ndone") + 5).split("$APPBASE/payload/webos25-hdmi").join(path.join(src, "kmod-out"))
      .split("/var/lib/webosbrew/dts25/hdmi").join(path.join(dir, "app-dts25", "hdmi"));
    ok(sh(["log() { :; }", replace, step].join("\n")), "Enable step 1d");
    files.forEach(function (f) { assert.strictEqual(read(path.join(dir, "app-dts25", "hdmi", f)), f); });
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("the HDMI toggle installs the current boot script, then stages, then runs its mode", function () {
  [true, false].forEach(function (on) {
    var script = service.w25HdmiSteps(on);
    assert.strictEqual(spawnSync("sh", ["-n"], {input: script, encoding: "utf8"}).status, 0);
    assert.ok(Buffer.byteLength(JSON.stringify({command: script})) < 110 * 1024, "fits a luna message");
    var install = script.indexOf("B64EOF\nchmod 0755");
    var stage = script.indexOf("/payload/webos25-hdmi/$f");
    var mode = script.indexOf("\nW25_HDMI=" + (on ? "on" : "off") + " sh ");
    assert.ok(install !== -1 && mode > install, "mode runs after the current script is written");
    assert.strictEqual(stage !== -1, on, "only Turn on stages the payload");
    if (on) assert.ok(stage > install && stage < mode);
    var ls = script.split("\n");
    var start = ls.findIndex(function (l) { return /^base64 -d <<'B64EOF' \| gzip -dc > /.test(l); });
    var dir = tmp("dts-hdmi-toggle-");
    try {
      // A TV that was never enabled has no dts25/ yet; the toggle creates it.
      var dest = path.join(dir, "never-enabled", "dts25");
      var block = ls.slice(start - 1, ls.indexOf("B64EOF", start) + 1).join("\n")
        .split("/var/lib/webosbrew/dts25").join(dest);
      ok(sh(block), "write the boot script");
      assert.strictEqual(fs.readFileSync(path.join(dest, "init_dts25.sh"), "utf8"), body);
    } finally {
      fs.rmSync(dir, {recursive: true, force: true});
    }
  });
});

test("the HDMI log shows the loader's output and the module's newest kernel lines", function () {
  var dir = tmp("dts-hdmi-log-");
  try {
    var log = ["[1.0] other: x"];
    for (var i = 1; i <= 90; i++) log.push("[" + i + ".0] earc_route: line " + i);
    write(path.join(dir, "dmesg.txt"), log.join("\n") + "\n");
    write(path.join(dir, "run/hdmi-watch.out"), "earc-route: PASSTHROUGH ACTIVE\n");
    var script = 'dmesg() { cat "' + path.join(dir, "dmesg.txt") + '"; }\n' +
      service.w25HdmiLogSteps().split("/run/dts25").join(path.join(dir, "run"));
    var r = sh(script);
    assert.strictEqual(r.status, 0);
    assert.strictEqual(r.stdout, "earc-route: PASSTHROUGH ACTIVE\n" + log.slice(11).join("\n") + "\n");
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test("the read-only probe reports the HDMI opt-in", function () {
  var r = sh(service.DETECT_PROBE);
  assert.strictEqual(r.status, 0, r.stderr);
  ["HDMI_FLAG=0", "HDMI_REFUSAL=kernel", "HDMI_RUNNING=0", "HDMI_KERNEL=", "HDMI_EXIT=", "HDMI_LOADER=", "HDMI_TOOLS="].forEach(function (k) {
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

test("the HDMI toggle reports refusals, a watcher that did not start, and a stop that needs a restart", function () {
  var k = {HDMI_KERNEL: "6.12.44-352", HDMI_TOOLS: "glasshouse"};
  return Promise.all([
    call(mockedService(k, "HDMI_REFUSED=kernel-6.12\nOK\n"), "setHdmiPassthrough", {enabled: true}).then(function (res) {
      assert.strictEqual(res.returnValue, false);
      assert.match(res.errorText, /Not supported yet on this kernel \(6\.12\.44-352\)/);
    }),
    call(mockedService(k, "HDMI_REFUSED=appdts\nOK\n"), "setHdmiPassthrough", {enabled: true}).then(function (res) {
      assert.match(res.errorText, /Turn on Show DTS tracks in apps first/);
    }),
    call(mockedService(k, "HDMI_RUNNING=1\nHDMI_LOADER=earc-route: PASSTHROUGH ACTIVE\nOK\n"), "setHdmiPassthrough", {enabled: true}).then(function (res) {
      assert.strictEqual(res.returnValue, true);
      assert.match(res.summary, /^HDMI passthrough on\. Glasshouse DTS forwarding is installed/);
    }),
    call(mockedService(k, "HDMI_RUNNING=0\nHDMI_LOADER=earc-route: REFUSED: another DTS tool is loaded: gh_dts_native (unload it first)\nOK\n"), "setHdmiPassthrough", {enabled: true}).then(function (res) {
      assert.strictEqual(res.returnValue, false);
      assert.match(res.errorText, /did not start: earc-route: REFUSED: another DTS tool is loaded/);
    }),
    call(mockedService(k, "WARN_HDMI=HDMI passthrough could not undo all of its changes. Restart the TV.\nHDMI_RUNNING=0\nOK\n"), "setHdmiPassthrough", {enabled: false}).then(function (res) {
      assert.strictEqual(res.returnValue, true);
      assert.match(res.summary, /off\. HDMI passthrough could not undo all of its changes\. Restart the TV\./);
    }),
    call(mockedService(k, "ERROR=HDMI passthrough did not stop; it is still on.\n"), "setHdmiPassthrough", {enabled: false}).then(function (res) {
      assert.strictEqual(res.returnValue, false);
      assert.match(res.errorText, /did not stop/);
    }),
    call(mockedService(k, "WARN_HDMI=HDMI passthrough could not undo all of its changes. Restart the TV.\nREG_REVERTED=1\nOK\n"), "disable").then(function (res) {
      assert.strictEqual(res.returnValue, true);
      assert.match(res.warning, /could not undo all of its changes/);
    })
  ]);
});

test("status reports the HDMI opt-in, its kernel, the watcher and what HDMI devices are offered", function () {
  var probes = {HDMI_FLAG: "1", HDMI_KERNEL: "5.4.268-329.ptl4tv.5", HDMI_REFUSAL: "", HDMI_RUNNING: "0", HDMI_EXIT: "1",
    HDMI_LOADER: "earc-route: REFUSED: lg-dts-core is enabled", HDMI_TOOLS: "lg-dts-core "};
  return Promise.all([
    call(mockedService(probes, "EDIDTYPE=TrueHD+dts\nEIM_HOOKED=0\n"), "status").then(function (s) {
      assert.strictEqual(s.hdmiRequested, true);
      assert.strictEqual(s.hdmiRunning, false);
      assert.strictEqual(s.hdmiExit, "1");
      assert.strictEqual(s.hdmiLoader, "earc-route: REFUSED: lg-dts-core is enabled");
      assert.strictEqual(s.hdmiRefusalText, "");
      assert.strictEqual(s.hdmiDtsOffered, true);
      assert.match(s.hdmiToolsNote, /^lg-dts-core is installed on this TV/);
    }),
    call(mockedService(Object.assign({}, probes, {HDMI_REFUSAL: "kernel-6.12", HDMI_KERNEL: "6.12.44-352", HDMI_TOOLS: ""}),
      "EDIDTYPE=TrueHD+dts\nEIM_HOOKED=1\n"), "status").then(function (s) {
      assert.match(s.hdmiRefusalText, /Not supported yet/);
      assert.strictEqual(s.hdmiDtsOffered, false, "the hook keeps DTS off the HDMI inputs");
      assert.strictEqual(s.hdmiToolsNote, "");
    })
  ]);
});
