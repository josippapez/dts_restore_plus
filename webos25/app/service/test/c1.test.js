"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var {service, sh, write, exe, loadService, ROOT} = require("./helpers");

/* The C1/G1 profile adds a GStreamer 1.16 dtsdec to the CX boot script. Both copies
 * of that script (the app's and the repo-root init_dts.sh) run here against a
 * temporary root: every absolute path they touch moves under it, and mount, umount,
 * chown and gst-inspect-1.0 are stubs that record what they were asked to do. */
var TV_PATHS = /\/var\/lib\/webosbrew|\/run\/dtsc1|\/usr\/lib\/gstreamer-1\.0|\/proc\/mounts|\/var\/tmp\/|\/etc\/gst\/|\/usr\/bin\/gst-inspect-1\.0|\/tmp\//g;

var SCRIPTS = {
  app: {
    body: function () { return service.cxInitScriptBody(); },
    dir: "var/lib/webosbrew/dtsenabler/cx",
    gst116: "var/lib/webosbrew/dtsenabler/cx/gst116"
  },
  root: {
    body: function () { return fs.readFileSync(path.join(ROOT, "init_dts.sh"), "utf8"); },
    dir: "var/lib/webosbrew/dts_restore",
    gst116: "var/lib/webosbrew/dts_restore/gst116"
  }
};

function tv(kind) {
  var r = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "dts-c1-")));
  var bin = path.join(r, "bin"), s = SCRIPTS[kind];
  ["tmp", "var/tmp", "proc", "usr/lib/gstreamer-1.0", s.dir].forEach(function (d) {
    fs.mkdirSync(path.join(r, d), {recursive: true});
  });
  write(path.join(r, "etc/gst/gstcool.conf"), "[sw_decoder]\ndts_audiodec=290\navdec_dca=0\n");
  write(path.join(r, "proc/mounts"), "");
  write(path.join(r, "registry.bin"), "stock");
  exe(path.join(bin, "mount"), 's=; t=; for a in "$@"; do s=$t; t=$a; done; echo "$s $t bind 0 0" >> "' + r + '/proc/mounts"');
  exe(path.join(bin, "umount"), 'for a in "$@"; do t=$a; done; grep -q " $t " "' + r + '/proc/mounts" || exit 1; ' +
    'grep -v " $t " "' + r + '/proc/mounts" > "' + r + '/m"; mv "' + r + '/m" "' + r + '/proc/mounts"');
  exe(path.join(bin, "chown"), "exit 0");
  exe(path.join(bin, "gst-inspect-1.0"), 'echo "${GST_PLUGIN_PATH_1_0-unset}" > "' + r + '/inspect.path"; ' +
    'printf "stock %s" "${GST_PLUGIN_PATH_1_0:-}" > "$GST_REGISTRY_1_0"');
  // One settings line, then the subscription stays open a few seconds.
  exe(path.join(bin, "luna-send"), 'echo \'{"settings":{"soundOutput":"external_optical","soundOutputDigital":"passThrough"}}\'; exec sleep 3');
  var script = path.join(r, s.dir, "init_dts.sh");
  write(script, s.body().replace(TV_PATHS, function (p) {
    return p === "/usr/bin/gst-inspect-1.0" ? "gst-inspect-1.0" : r + p;
  }), 0o755);
  return {
    root: r,
    gst116: path.join(r, s.gst116),
    stage: function () { write(path.join(r, s.gst116, "libgstdtsdec.so"), "dtsdec"); },
    run: function (env) {
      var res = sh('sh "' + script + '"', Object.assign({PATH: bin + ":" + process.env.PATH,
        GST_REGISTRY_1_0: path.join(r, "registry.bin")}, env || {}));
      assert.equal(res.status, 0, res.stderr);
      return res;
    },
    read: function (p) { return fs.existsSync(path.join(r, p)) ? fs.readFileSync(path.join(r, p), "utf8") : null; },
    done: function () { fs.rmSync(r, {recursive: true, force: true}); }
  };
}

Object.keys(SCRIPTS).forEach(function (kind) {
  test(kind + " boot script: the C1 dtsdec is scanned and ranked above avdec_dca", function () {
    var t = tv(kind);
    try {
      t.stage();
      t.run({GST_PLUGIN_PATH: "/lg/extra"});
      assert.match(t.read("tmp/gstcool.conf"), /\navdec_dca=290\ndtsdec=300\n/);
      // GST_PLUGIN_PATH_1_0 replaces GST_PLUGIN_PATH, so LG's own entry must survive.
      assert.equal(t.read("inspect.path"), t.gst116 + ":/lg/extra\n");
      assert.match(t.read("proc/mounts"), / \S*\/etc\/gst\/gstcool\.conf /);
      assert.match(t.read("proc/mounts"), / \S*\/registry\.bin /);
    } finally { t.done(); }
  });

  test(kind + " boot script: without the C1 dtsdec it does what it did before", function () {
    var t = tv(kind);
    try {
      t.run({GST_PLUGIN_PATH: "/lg/extra"});
      assert.equal(t.read("tmp/gstcool.conf").split("\n[downmix]")[0],
        "[sw_decoder]\ndts_audiodec=290\navdec_dca=290\n");
      assert.equal(t.read("inspect.path"), "unset\n");
    } finally { t.done(); }
  });

  test(kind + " boot script: a rank file and registry from an install without dtsdec are rebuilt once", function () {
    var t = tv(kind);
    try {
      t.stage();
      write(path.join(t.root, "tmp/gstcool.conf"), "avdec_dca=290\n");
      write(path.join(t.root, "proc/mounts"), "x " + path.join(t.root, "etc/gst/gstcool.conf") + " bind 0 0\n" +
        "x " + path.join(t.root, "registry.bin") + " bind 0 0\n");
      t.run();
      assert.match(t.read("tmp/gstcool.conf"), /\navdec_dca=290\ndtsdec=300\n/);
      assert.ok(t.read("inspect.path"), "the registry was not rebuilt");
      // Now both carry dtsdec: a second run leaves them alone.
      write(path.join(t.root, "registry.bin"), "stock " + t.gst116 + "/libgstdtsdec.so");
      fs.rmSync(path.join(t.root, "inspect.path"));
      var gstcool = t.read("tmp/gstcool.conf");
      t.run();
      assert.equal(t.read("inspect.path"), null, "the registry was rebuilt again");
      assert.equal(t.read("tmp/gstcool.conf"), gstcool);
    } finally { t.done(); }
  });

  test(kind + " boot script: no sound-output watcher without the passthrough opt-in", function () {
    var t = tv(kind);
    try {
      t.stage();
      t.run();
      assert.equal(t.read("run/dtsc1/watcher.pid"), null);
    } finally { t.done(); }
  });

  // The watcher takes its own pid from /proc/self/stat, as on the TV.
  test(kind + " boot script: with the opt-in the watcher writes the sound output for dtsdec",
      {skip: !fs.existsSync("/proc/self/stat") && "needs Linux /proc"}, function () {
    var t = tv(kind);
    try {
      t.stage();
      write(path.join(t.root, "var/lib/webosbrew/dtsc1/passthrough"), "");
      t.run();
      var out = null;
      for (var i = 0; i < 40 && !out; i++) {
        out = t.read("run/dtsc1/sound-output");
        if (!out) sh("sleep 0.05");
      }
      // "<watcher pid> <soundOutput> <soundOutputDigital>", the format dtsdec reads.
      var m = /^(\d+) external_optical passThrough\n$/.exec(out || "");
      assert.ok(m, "sound-output: " + JSON.stringify(out));
      assert.equal(t.read("run/dtsc1/watcher.pid"), m[1] + "\n");
      fs.rmSync(path.join(t.root, "var/lib/webosbrew/dtsc1/passthrough"));
    } finally { t.done(); }
  });
});

/* The repo-root installer, run from a copy of the repository layout it expects. */
function installer(gstVersion) {
  var t = tv("root");
  var src = path.join(t.root, "src"), bin = path.join(t.root, "bin");
  ["install.sh", "uninstall.sh", "init_dts.sh", "downmix.conf"].forEach(function (f) {
    write(path.join(src, f), fs.readFileSync(path.join(ROOT, f), "utf8").replace(TV_PATHS, function (p) {
      return p === "/usr/bin/gst-inspect-1.0" ? "gst-inspect-1.0" : t.root + p;
    }), 0o755);
  });
  ["libgstlibav.so", "libgstmatroska.so"].forEach(function (f) { write(path.join(src, "gst", f), f); });
  write(path.join(src, "webos25/restore/gst116-out/libgstdtsdec.so"), "dtsdec-116");
  fs.mkdirSync(path.join(t.root, "var/lib/webosbrew/init.d"), {recursive: true});
  exe(path.join(bin, "id"), "echo 0");
  exe(path.join(bin, "nyx-cmd"), "echo OLED65C17LB");
  exe(path.join(bin, "gst-inspect-1.0"), '[ "${1:-}" = --version ] && { echo "GStreamer ' + gstVersion + '"; exit 0; }; ' +
    'printf "stock %s" "${GST_PLUGIN_PATH_1_0:-}" > "$GST_REGISTRY_1_0"');
  t.sh = function (cmd) {
    var res = sh("cd '" + src + "' && " + cmd, {PATH: bin + ":" + process.env.PATH,
      GST_REGISTRY_1_0: path.join(t.root, "registry.bin")});
    assert.equal(res.status, 0, res.stdout + res.stderr);
    return res.stdout;
  };
  return t;
}

test("install.sh stages the 1.16 dtsdec and the passthrough opt-in on GStreamer 1.16", function () {
  var t = installer("1.16.2");
  try {
    t.sh("bash install.sh -y --passthrough");
    assert.equal(t.read("var/lib/webosbrew/dts_restore/gst116/libgstdtsdec.so"), "dtsdec-116");
    assert.equal(t.read("var/lib/webosbrew/dtsc1/passthrough"), "");
    assert.match(t.read("tmp/gstcool.conf"), /\navdec_dca=290\ndtsdec=300\n/);
    // Installing again without the option turns passthrough off.
    t.sh("bash install.sh -y");
    assert.equal(t.read("var/lib/webosbrew/dtsc1/passthrough"), null);
    t.sh("bash uninstall.sh");
    assert.equal(t.read("var/lib/webosbrew/dts_restore"), null);
  } finally { t.done(); }
});

test("install.sh keeps the CX-only install on other GStreamer versions", function () {
  var t = installer("1.14.4");
  try {
    assert.match(t.sh("bash install.sh -y --passthrough"), /--passthrough needs webOS 6/);
    assert.equal(t.read("var/lib/webosbrew/dts_restore/gst116"), null);
    assert.equal(t.read("var/lib/webosbrew/dtsc1/passthrough"), null);
    assert.doesNotMatch(t.read("tmp/gstcool.conf"), /dtsdec=/);
  } finally { t.done(); }
});

test("Enable stages the GStreamer 1.16 dtsdec on the C1 profile only", function () {
  var c1 = service.cxEnable(true), cx = service.cxEnable(false);
  assert.match(c1, /cp -f "\$APPBASE\/payload\/c1\/libgstdtsdec\.so" "\/var\/lib\/webosbrew\/dtsenabler\/cx\/gst116\/"/);
  assert.equal(cx.indexOf("payload/c1"), -1);
  // A CX TV, or a C1 whose copy failed, keeps no stale 1.16 plugin around.
  [c1, cx].forEach(function (s) { assert.match(s, /rm -rf "\/var\/lib\/webosbrew\/dtsenabler\/cx\/gst116"/); });
});

test("C1 passthrough counts only an optical or ARC receiver on Pass Through", function () {
  assert.equal(service.c1PtReceiver("external_optical passThrough"), true);
  assert.equal(service.c1PtReceiver("external_arc passThrough"), true);
  // The 1.16 dtsdec decodes on Auto, unlike the webOS 25 one.
  assert.equal(service.c1PtReceiver("external_optical auto"), false);
  assert.equal(service.c1PtReceiver("tv_speaker passThrough"), false);
});

test("setPassthrough on a C1 runs the C1 steps and reports the receiver", function () {
  var script = null;
  var mocked = loadService({
    detectProfile: function () { return Promise.resolve({profile: "webos6-armel-gst116", probes: {}}); },
    rootExec: function (s) { script = s; return Promise.resolve({stdout: "PT_OUT=external_arc passThrough\nOK\n", stderr: ""}); },
    logActionResult: function () {}
  });
  return new Promise(function (resolve) {
    mocked.handlers.setPassthrough({payload: {enabled: true}, respond: resolve});
  }).then(function (response) {
    assert.equal(response.returnValue, true, response.errorText);
    assert.equal(response.receiver, true);
    assert.equal(script, service.c1PassthroughSteps(true));
    assert.match(script, /: > "\/var\/lib\/webosbrew\/dtsc1\/passthrough"/);
  });
});
