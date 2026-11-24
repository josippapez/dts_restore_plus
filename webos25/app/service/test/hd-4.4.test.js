"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var os = require("node:os");
var {service, ROOT, sh, write, exe} = require("./helpers");

var HD44_INIT = "/var/lib/webosbrew/dts25/4.4/init_4.4.sh";

/* 4.4 DTS-HD is an experimental opt-in on top of the CX restore: the steps refuse
 * anything but the analysed kernel, stage the 4.4 payload, chain init_4.4.sh after
 * the CX boot hook, and turning it off never tries to unload the 4.4 module. */
test("4.4 DTS-HD steps refuse other kernels and a TV without the DTS restore", function () {
  var on = service.hd44Steps(true);
  assert.strictEqual(spawnSync("sh", ["-n"], { input: on, encoding: "utf8" }).status, 0);
  assert.ok(on.includes('[ "$(uname -r)" = 4.4.84-229.kcl4tv.6 ] ||'));
  assert.ok(on.indexOf("/var/lib/webosbrew/init.d/restore_dts") < on.indexOf('cp -f'), "needs Enable first");
  ["libgstdtsdec.so", "earc_route-4.4.ko", "earc-route-load.sh", "init_4.4.sh"].forEach(function (f) {
    assert.ok(on.includes(f), "stages " + f);
  });
  assert.ok(on.indexOf("/enabled\"") < on.indexOf('sh "' + HD44_INIT + '"'), "marker before the apply");
});

test("4.4 DTS-HD off stops the watcher and never unloads the 4.4 module", function () {
  var off = service.hd44Steps(false);
  assert.strictEqual(spawnSync("sh", ["-n"], { input: off, encoding: "utf8" }).status, 0);
  assert.ok(off.includes("HD44=stop"));
  assert.ok(!/rmmod|delete_module/.test(off), "the 4.4 build has no exit");
});

test("Enable keeps the 4.4 chain, Disable stops it before the hook, Uninstall removes it", function () {
  var enable = service.cxEnable();
  assert.ok(enable.includes('[ -f "/var/lib/webosbrew/dts25/4.4/enabled" ] && sed -i'));
  var disable = service.cxDisable();
  var stop = disable.indexOf("HD44=stop");
  assert.ok(stop >= 0 && stop < disable.indexOf('rm -f "/var/lib/webosbrew/init.d/restore_dts"'));
  assert.ok(service.cxUninstall().includes('rm -rf "/var/lib/webosbrew/dts25/4.4"'));
});

// A C1 tester updated the app and pressed Enable: the boot chain kept running 2.86.1's init_4.4.sh.
test("Enable with DTS-HD on refreshes the staged 4.4 files from the updated app", function () {
  var enable = service.cxEnable(true);
  var refresh = service.hd44RefreshSteps();
  var at = enable.indexOf(refresh);
  assert.ok(at >= 0 && at < enable.indexOf('sh "/var/lib/webosbrew/dtsenabler/cx/init_dts.sh"\n'), "refresh before the apply");
  var r = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "dts-hd44-refresh-")));
  var files = ["libgstdtsdec.so", "earc_route-4.4.ko", "earc-route-load.sh", "init_4.4.sh"];
  var staged = path.join(r, "var/lib/webosbrew/dts25/4.4");
  try {
    files.forEach(function (f) { write(path.join(r, "app/payload/4.4", f), "new " + f); write(path.join(staged, f), "old " + f); });
    var script = 'log() { :; }\nAPPBASE="' + r + '/app"\n' + refresh.split("/var/lib/webosbrew").join(r + "/var/lib/webosbrew");
    assert.strictEqual(sh(script).status, 0);
    files.forEach(function (f) { assert.strictEqual(fs.readFileSync(path.join(staged, f), "utf8"), "old " + f, "DTS-HD off: untouched"); });
    write(path.join(staged, "enabled"), "");
    assert.strictEqual(sh(script).status, 0);
    files.forEach(function (f) { assert.strictEqual(fs.readFileSync(path.join(staged, f), "utf8"), "new " + f); });
  } finally { fs.rmSync(r, {recursive: true, force: true}); }
});

test("init_4.4.sh: off by default, exact kernel, never writes /usr, exits 0", function () {
  var script = fs.readFileSync(path.join(ROOT, "webos25/restore/init_4.4.sh"), "utf8");
  assert.strictEqual(spawnSync("sh", ["-n", "-c", script], { encoding: "utf8" }).status, 0);
  var gate = script.indexOf('\n[ -f "$HD44_DIR/enabled" ] || exit 0\n');
  assert.ok(gate >= 0 && gate < script.indexOf('earc-route-load.sh" --dts-hd'), "the marker gates the module load");
  assert.ok(script.includes('[ "$(uname -r)" = 4.4.84-229.kcl4tv.6 ]'));
  assert.ok(!/remount,rw|> *\/usr\//.test(script), "LG's /usr stays untouched");
  assert.ok(!/rmmod/.test(script));
  var lines = script.trim().split("\n");
  assert.strictEqual(lines[lines.length - 1], "exit 0");
  // A real run with no marker does nothing at all.
  var r = spawnSync("sh", ["-c", script.replace(/^HD44_DIR=.*$/m, "HD44_DIR=/nonexistent-hd44-dir")], { encoding: "utf8" });
  assert.strictEqual(r.status, 0);
  assert.strictEqual(r.stdout + r.stderr, "");
});

// The CX boot hook has already bound its registry when init_4.4.sh runs, and the rescan
// replaces the file under that bind, so only a fresh bind makes dtsdec reach LG's player.
// The C1's busybox timeout (before 1.30) takes -t SECS and runs "60" as the program otherwise.
var TIMEOUTS = {
  "busybox before 1.30": '[ "$1" = -t ] || { echo "timeout: can\'t execute \'$1\': No such file or directory" >&2; exit 127; }; shift 2; exec "$@"',
  "busybox 1.30 and coreutils": 'case "$1" in -*) echo "timeout: invalid option -- \'${1#-}\'" >&2; exit 1;; esac; shift; exec "$@"',
};
Object.keys(TIMEOUTS).forEach(function (flavour) {
test("init_4.4.sh binds the rescanned registry over one the CX hook already bound (" + flavour + " timeout)", function () {
  var r = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "dts-hd44-")));
  var bin = path.join(r, "bin"), live = path.join(r, "registry.bin");
  try {
    exe(path.join(bin, "timeout"), TIMEOUTS[flavour]);
    write(path.join(r, "var/lib/webosbrew/dts25/4.4/enabled"), "");
    write(path.join(r, "var/lib/webosbrew/dts25/4.4/libgstdtsdec.so"), "dtsdec-4.4");
    write(path.join(r, "var/lib/webosbrew/dts25/4.4/earc-route-load.sh"), "echo loaded");
    write(live, "stock");
    write(path.join(r, "proc/mounts"), "x " + live + " bind 0 0\n");
    exe(path.join(bin, "uname"), "echo 4.4.84-229.kcl4tv.6");
    exe(path.join(bin, "mount"), 'for a in "$@"; do s=$t; t=$a; done; echo "$s $t" >> "' + r + '/binds"');
    exe(path.join(bin, "chown"), "exit 0");
    exe(path.join(bin, "luna-send"), "exit 0");
    exe(path.join(bin, "gst-inspect-1.0"), '[ "${GST_REGISTRY_UPDATE:-}" = yes ] && echo with-dtsdec > "$GST_REGISTRY_1_0"; [ -f "$GST_REGISTRY_1_0" ]');
    var script = fs.readFileSync(path.join(ROOT, "webos25/restore/init_4.4.sh"), "utf8")
      .replace(/\/usr\/bin\/gst-inspect-1\.0/g, "gst-inspect-1.0")
      .replace(/\/var\/lib\/webosbrew|\/run\/dts25|\/proc\/mounts|\/tmp\/|\/usr\/lib\/gstreamer-1\.0/g, function (p) { return r + p; });
    fs.mkdirSync(path.join(r, "tmp"));
    write(path.join(r, "init_4.4.sh"), script, 0o755);
    var res = sh('sh "' + r + '/init_4.4.sh"', {PATH: bin + ":" + process.env.PATH, GST_REGISTRY_1_0: live});
    assert.strictEqual(res.status, 0, res.stderr);
    var reg = path.join(r, "run/dts25/registry.bin");
    assert.strictEqual(fs.readFileSync(reg, "utf8"), "with-dtsdec\n");
    assert.strictEqual(fs.existsSync(path.join(r, "binds")) && fs.readFileSync(path.join(r, "binds"), "utf8"), reg + " " + live + "\n");
  } finally { fs.rmSync(r, {recursive: true, force: true}); }
});
});
