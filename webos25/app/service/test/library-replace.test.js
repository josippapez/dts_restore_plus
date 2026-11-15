"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var {service, sh, ROOT} = require("./helpers");

/* A library a running process has mapped must never be rewritten in place. `cp -f`
 * onto an existing file truncates and rewrites the same inode, and truncation drops
 * every process's relocated private pages of it: on a C5, a reinstall that rewrote
 * libedidaudio.so under a hooked eim crashed eim (pc 0x4d4, an unrelocated PLT slot)
 * the next time it built an EDID. Staging goes through a temp file and a rename. */
var INSTALL = fs.readFileSync(path.join(ROOT, "webos25/restore/install.sh"), "utf8");
var UNINSTALL = fs.readFileSync(path.join(ROOT, "webos25/restore/uninstall.sh"), "utf8");

function scripts() {
  return {
    "install.sh": INSTALL, "uninstall.sh": UNINSTALL,
    "w25Enable": service.w25Enable(false), "w25AppDtsSteps(on)": service.w25AppDtsSteps(true),
    "c2Enable": service.c2Enable(true), "c2InitScriptBody": service.c2InitScriptBody()
  };
}

// Every cp destination, with the command's start anchored so arguments of other
// commands are not mistaken for one.
function copies(text) {
  var re = /(?:^|[;&|{(]|\bthen|\bdo|\belse|\bif)\s*(?:"\$C2_CP"|cp)(?:\s+-\w+)*\s+("[^"]*"|[^\s;&|]+)\s+("[^"]*"|[^\s;&|]+)/gm;
  var out = [], m;
  while ((m = re.exec(text))) out.push({src: m[1], dest: m[2].replace(/^"|"$/g, "")});
  return out;
}

// Destinations that are data, not loaded code: the media registry (committed with
// `cp -f` on purpose, see init_dts25.sh) and the generated /etc overrides.
var DATA = ["$CFG_OVR", "$GC_OVR", "$REG", "$REG_TARGET",
  "/var/lib/webosbrew/truehd/codec_capability.json", "/var/lib/webosbrew/truehd/gstcool.conf"];

test("no installer or app script copies a file straight onto a staged library path", function () {
  var s = scripts();
  Object.keys(s).forEach(function (name) {
    copies(s[name]).forEach(function (c) {
      if (DATA.indexOf(c.dest) >= 0) return;
      assert.match(c.dest, /\.new$/, name + ": cp " + c.src + " " + c.dest + " writes the target in place");
    });
  });
  // The scan found the copies it is meant to guard (a regex that stopped matching
  // would pass vacuously).
  assert.ok(copies(s["w25AppDtsSteps(on)"]).some(function (c) { return /libedidaudio\.so\.new$/.test(c.dest); }));
  assert.ok(copies(s.c2Enable).length >= 2);
});

function helperFrom(text) {
  var m = /^(replace_file\(\) \{[\s\S]*?\})/m.exec(text);
  assert.ok(m, "replace_file() is defined");
  return m[1];
}

[["install.sh", function () { return INSTALL; }], ["w25Enable", function () { return service.w25Enable(false); }]].forEach(function (src) {
  test(src[0] + ": replace_file swaps in a new inode and leaves an open handle on the old one", function () {
    var fn = helperFrom(src[1]());
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-replace-"));
    var dest = path.join(dir, "lib.so"), payload = path.join(dir, "payload.so");
    fs.writeFileSync(dest, "old");
    fs.writeFileSync(payload, "new");
    var before = fs.statSync(dest).ino;
    var held = fs.openSync(dest, "r");   // stands in for a process that has it mapped
    var r = sh(fn + '\nreplace_file "' + payload + '" "' + dest + '"');
    assert.equal(r.status, 0, r.stderr);
    assert.equal(fs.readFileSync(dest, "utf8"), "new");
    assert.notEqual(fs.statSync(dest).ino, before, "the target must be a new inode");
    var buf = Buffer.alloc(3); fs.readSync(held, buf, 0, 3, 0); fs.closeSync(held);
    assert.equal(buf.toString(), "old", "the open file must keep its old contents");
    assert.equal(fs.existsSync(dest + ".new"), false);

    // A soname symlink stays a symlink.
    fs.symlinkSync("lib.so", path.join(dir, "link-src"));
    r = sh(fn + '\nreplace_file "' + path.join(dir, "link-src") + '" "' + path.join(dir, "lib.so.1") + '"');
    assert.equal(r.status, 0, r.stderr);
    assert.equal(fs.readlinkSync(path.join(dir, "lib.so.1")), "lib.so");

    // A failed copy keeps the old file and leaves no temp file behind.
    var kept = fs.statSync(dest).ino;
    r = sh(fn + '\nreplace_file "' + path.join(dir, "absent.so") + '" "' + dest + '"');
    assert.notEqual(r.status, 0);
    assert.equal(fs.statSync(dest).ino, kept);
    assert.equal(fs.readFileSync(dest, "utf8"), "new");
    assert.equal(fs.existsSync(dest + ".new"), false);
    fs.rmSync(dir, {recursive: true, force: true});
  });
});
