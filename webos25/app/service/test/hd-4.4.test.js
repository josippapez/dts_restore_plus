"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, ROOT} = require("./helpers");

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
