"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var path = require("node:path");
var {service, loadService, C, sh, write, exe, fixture, run, ok} = require("./helpers");

/* Issue #37: an OLED55CS9LA on 13.30.56 has LG's own dts_audiodec. Binding only the
 * demuxers made DTS play there, so its set must bind neither libgstlibav nor
 * gstcool.conf, while the two legacy C2/G2 sets keep doing exactly that. */
var NATIVE = C.sets.filter(function (t) { return t.native; })[0];
var TS = "libgstmpegtsdemux.so";
var BASELINE_KEYS = ["hardware_id", "product_id", "board_type", "firmware", "webos", "gstreamer",
  "libgstlibav", "libgstisomp4", "libgstmatroska", "gstcool", "registry", "init_sha256", "hook_sha256"];

function tv(f, id, hashes) {
  exe(f.command("nyx-cmd"), 'case "$*" in *hardware_id) echo ' + id.hw + ';; *product_id) echo ' + id.pid + ';; *board_type) echo ' + id.board + ';; *webos_manufacturing_version) echo ' + id.fw + ';; *webos_release) echo ' + id.webos + ';; esac');
  exe(f.command("sha256sum"), 'case "$1" in *libgstlibav.so) h="' + hashes.libav + '";; *libgstisomp4.so) h="' + hashes.iso + '";; *libgstmatroska.so) h="' + hashes.mkv + '";; *gstcool.conf) h="' + "a".repeat(64) + '";; *) exec /usr/bin/shasum -a 256 "$1";; esac; echo "$h  $1"');
  // Logs every element the registry proof asks for.
  exe(f.command("gst-inspect-1.0"), '[ "$1" = --version ] && { echo "GStreamer ' + id.gst + '"; exit 0; }; if [ -n "${GST_REGISTRY_1_0:-}" ] && [ "$#" -eq 0 ]; then printf registry > "$GST_REGISTRY_1_0" || exit 1; fi; [ "$#" -eq 0 ] || echo "$1" >> "' + f.dir + '/proof.log"; exit 0');
  write(path.join(f.payloadts, TS), "payload-" + TS);
  write(path.join(f.target, TS), "stock-" + TS);
}
function nativeTv() {
  assert.ok(NATIVE, "a native C2 set must be accepted");
  var f = fixture();
  tv(f, {hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS9LA", board: "O22_DVB", fw: "13.30.56", webos: "8.3.0", gst: "1.18.5"}, NATIVE);
  return f;
}
function legacyTv(i) {
  var f = fixture();
  tv(f, i === 0
    ? {hw: "HE_DTV_W22O_AFABATAA", pid: "OLED65C2PUA", board: "o22", fw: "04.40.93.01", webos: "7.4.0", gst: "1.18.2"}
    : {hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA", board: "O22_DVB", fw: "23.25.55", webos: "9.2.2", gst: "1.18.5"}, C.sets[i]);
  return f;
}
function mounted(f) {
  return fs.readFileSync(f.mountinfo, "utf8").trim().split("\n").slice(1).map(function (l) { return l.split(" ")[4]; }).sort();
}
function proof(f) {
  var p = path.join(f.dir, "proof.log");
  var lines = fs.existsSync(p) ? fs.readFileSync(p, "utf8").trim().split("\n") : [];
  fs.rmSync(p, {force: true});
  return lines.join(" ");
}
function kvOf(stdout) {
  var kv = {};
  stdout.split("\n").forEach(function (l) { var at = l.indexOf("="); if (at > 0) kv[l.slice(0, at)] = l.slice(at + 1); });
  return kv;
}
function demuxerTargets(f, withLibav) {
  return [f.registry].concat(withLibav ? [path.join(f.target, "libgstlibav.so"), f.gstcool] : [])
    .concat(["libgstisomp4.so", "libgstmatroska.so", "libgstisomp4_1_8.so", TS].map(function (n) { return path.join(f.target, n); })).sort();
}

test("the native CS9 set matches itself, rejects chimeras, and stays a two-step opt-in", function () {
  assert.ok(NATIVE, "a native C2 set must be accepted");
  assert.equal(C.matchSet(NATIVE.libav, NATIVE.iso, NATIVE.mkv), NATIVE);
  assert.match(NATIVE.label, /13\.30\.56/);
  assert.equal(C.sets.filter(function (t) { return t.native; }).length, 1, "only the CS9 set is native");
  C.sets.filter(function (t) { return !t.native; }).forEach(function (o) {
    assert.equal(C.matchSet(NATIVE.libav, o.iso, o.mkv), null);
    assert.equal(C.matchSet(o.libav, NATIVE.iso, NATIVE.mkv), null);
    assert.equal(C.matchSet(NATIVE.libav, NATIVE.iso, o.mkv), null);
    assert.equal(C.matchSet(o.libav, o.iso, NATIVE.mkv), null);
  });
  var cv = service.compatVerdict(C.C2, {C2_HASH_TOOL: "1", C2_GSTCOOL_SHA256: "a".repeat(64),
    C2_LIBAV_SHA256: NATIVE.libav, C2_ISOMP4_SHA256: NATIVE.iso, C2_MATROSKA_SHA256: NATIVE.mkv});
  assert.equal(cv.verdict, "unverified", "never verified");
  assert.equal(cv.canForce, true, "offered only through the explicit opt-in");
  assert.match(cv.verdictReason, /LG's own DTS decoder/);

  // The probe accepts exactly 13.30.56 / 8.3.0 / 1.18.5 and nothing near it.
  var probe = service.DETECT_PROBE;
  var block = probe.slice(probe.indexOf("# --- PROFILE SELECTION ---"), probe.indexOf('echo "PROFILE=$PROFILE"'));
  function profileFor(fw, webos, gst) {
    var r = sh(["set -u", "GST_VERSION=" + gst, 'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
      "HARDWARE_ID=HE_DTV_W22O_AFABATPU", "PRODUCT_ID=OLED55CS9LA", "BOARD_TYPE=O22_DVB",
      "WEBOS_RELEASE=" + webos, "WEBOS_MANUFACTURING_VERSION=" + fw, "LOADER=ld-linux.so.3", "FLOAT_ABI=soft",
      "HAS_DTS_AUDIODEC=yes", "DCA_RANK=0"].join("\n") + "\n" + block + '\nprintf "%s" "$PROFILE"');
    assert.equal(r.status, 0, r.stderr);
    return r.stdout.trim().split("\n").pop().trim();
  }
  assert.equal(profileFor("13.30.56", "8.3.0", "1.18.5"), C.C2);
  assert.equal(profileFor("13.30.56.01", "8.3.0", "1.18.5"), "webos22-o22-c2-diagnostic");
  assert.equal(profileFor("13.30.56", "8.3.1", "1.18.5"), "webos22-o22-c2-diagnostic");
  assert.equal(profileFor("13.30.56", "8.3.0", "1.18.2"), "webos22-o22-c2-diagnostic");
});

test("native enable binds the demuxers and registry with a dts_audiodec proof, never libgstlibav or gstcool.conf", function () {
  var f = nativeTv(), o = f.overrides();
  var r = run(f, service.c2Enable(true, o));
  ok(r, "native enable");
  assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.match(r.stdout, /TS_BOUND=1/);
  assert.deepEqual(mounted(f), demuxerTargets(f, false));
  assert.equal(proof(f), "dts_audiodec qtdemux matroskademux tsdemux");
  assert.equal(fs.existsSync(path.join(f.gst, "libgstlibav.so")), false, "libgstlibav must not be staged");
  assert.equal(fs.existsSync(o.configSource), false, "gstcool.conf must not be rewritten");
  assert.equal(fs.readFileSync(f.gstcool, "utf8"), "avdec_dca=0\n");
  var keys = fs.readFileSync(o.baseline, "utf8").trim().split("\n").map(function (l) { return l.split("=")[0]; });
  assert.deepEqual(keys, BASELINE_KEYS, "the mode adds no baseline key");
  assert.match(fs.readFileSync(o.baseline, "utf8"), new RegExp("^libgstlibav=" + NATIVE.libav + "$", "m"));
});

test("native boot re-applies the demuxers only, decided by the persisted baseline", function () {
  var f = nativeTv(), o = f.overrides();
  ok(run(f, service.c2Enable(true, o)), "native enable before boot");
  proof(f);
  write(f.mountinfo, "1 0 0:1 / / rw - fake root rw\n");
  var r = run(f, 'exec sh "' + o.init + '"');
  ok(r, "native boot");
  assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.deepEqual(mounted(f), demuxerTargets(f, false));
  assert.equal(proof(f), "dts_audiodec qtdemux matroskademux tsdemux");
  assert.equal(fs.existsSync(path.join(f.gst, "libgstlibav.so")), false);
  assert.equal(fs.existsSync(o.configSource), false);
  assert.equal(fs.existsSync(o.recovery), false);
});

test("native status is complete without libav/config binds, and the self-test only demuxes", function () {
  var f = nativeTv(), o = f.overrides();
  ok(run(f, service.c2Enable(true, o)), "native enable before status");
  var r = run(f, service.c2StatusProbe(o));
  ok(r, "native status");
  var kv = kvOf(r.stdout);
  assert.equal(kv.NATIVE, "1");
  assert.equal(kv.LIBAV, "none"); assert.equal(kv.CONFIG, "none");
  assert.equal(service.c2StatusBindsComplete(kv), true, "a native install is complete without libav/config");
  assert.equal(service.c2StatusBindsComplete(Object.assign({}, kv, {MKV: "none"})), false);
  assert.equal(service.c2StatusBindsComplete(Object.assign({}, kv, {LIBAV: "owned"})), false, "libav must stay stock");
  assert.equal(service.c2StatusBindsComplete(Object.assign({}, kv, {NATIVE: "0"})), false, "legacy still needs libav and config");

  // A live native install is the app's own, not drift, so re-enable stays allowed.
  var probes = {C2_HASH_TOOL: "1", C2_OWNED: "1", C2_BASELINE_VALID: "1", C2_FOREIGN: "0", C2_INSPECT_OK: "1",
    HARDWARE_ID: "HE_DTV_W22O_AFABATPU", PRODUCT_ID: "OLED55CS9LA", BOARD_TYPE: "O22_DVB",
    WEBOS_MANUFACTURING_VERSION: "13.30.56", WEBOS_RELEASE: "8.3.0", GST_VERSION: "1.18.5",
    C2_FP_HARDWARE_ID: "HE_DTV_W22O_AFABATPU", C2_FP_PRODUCT_ID: "OLED55CS9LA", C2_FP_BOARD_TYPE: "O22_DVB",
    C2_FP_FIRMWARE: "13.30.56", C2_FP_WEBOS: "8.3.0", C2_FP_GSTREAMER: "1.18.5",
    C2_FP_LIBGSTLIBAV: NATIVE.libav, C2_FP_LIBGSTISOMP4: NATIVE.iso, C2_FP_LIBGSTMATROSKA: NATIVE.mkv, C2_FP_GSTCOOL: "a".repeat(64),
    C2_LIBAV_SHA256: NATIVE.libav, C2_ISOMP4_SHA256: "b".repeat(64), C2_MATROSKA_SHA256: "c".repeat(64), C2_GSTCOOL_SHA256: "a".repeat(64),
    C2_MOUNT_LIBAV: "none", C2_MOUNT_ISO: "owned", C2_MOUNT_MKV: "owned", C2_MOUNT_CONFIG: "none"};
  assert.equal(service.compatVerdict(C.C2, probes).verdict, "forced");

  exe(f.command("gst-launch-1.0"), 'echo "$*" >> "' + f.dir + '/launch.log"; [ "${FAIL_LAUNCH:-}" = 1 ] && exit 1; exit 0');
  r = run(f, service.c2SelfTest(o));
  ok(r, "native self-test");
  assert.match(r.stdout, /^mp4=PASS:demux$/m); assert.match(r.stdout, /^ts=PASS:demux$/m); assert.match(r.stdout, /^m2ts=PASS:demux$/m);
  var launches = fs.readFileSync(path.join(f.dir, "launch.log"), "utf8").trim().split("\n");
  assert.equal(launches.length, 3);
  launches.forEach(function (l) {
    assert.match(l, /(qtdemux|tsdemux) ! audio\/x-dts ! fakesink$/, l);
    assert.doesNotMatch(l, /dts_audiodec|avdec_dca|wavenc/, "no decode in the native self-test");
  });
  r = run(f, service.c2SelfTest(o), {FAIL_LAUNCH: "1"});
  assert.match(r.stdout, /^mp4=FAIL:demux$/m);

  var selfTest = run(f, service.c2SelfTest(o)).stdout, status = run(f, service.c2StatusProbe(o)).stdout;
  var m = loadService({
    detectProfile: function () { return Promise.resolve({profile: C.C2, probes: probes}); },
    rootExec: function (cmd) { return Promise.resolve({stdout: cmd.indexOf("gst-launch-1.0") >= 0 ? selfTest : status, stderr: ""}); }
  });
  return Promise.all([
    new Promise(function (resolve) { m.handlers.test({payload: {}, respond: resolve}); }).then(function (res) {
      assert.equal(res.pass, true);
      assert.equal(res.results.mp4.demuxOnly, true);
      assert.equal(res.results.ts.verdict, "PASS");
    }),
    new Promise(function (resolve) { m.handlers.status({payload: {}, respond: resolve}); }).then(function (s) {
      assert.equal(s.c2Native, true);
      assert.equal(s.active, true);
      assert.match(s.mechanism, /LG's own DTS decoder/);
      assert.doesNotMatch(s.mechanism, /legacy/);
    })
  ]);
});

test("disable, re-enable, and uninstall of a native install clean up", function () {
  var f = nativeTv(), o = f.overrides();
  ok(run(f, service.c2Enable(true, o)), "native enable");
  var r = run(f, service.c2Disable(false, o));
  ok(r, "native disable"); assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.deepEqual(mounted(f), []);
  assert.equal(fs.existsSync(o.hook), false); assert.equal(fs.existsSync(o.owner), true);
  proof(f);
  r = run(f, service.c2Enable(false, o));
  ok(r, "native re-enable"); assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.deepEqual(mounted(f), demuxerTargets(f, false));
  assert.equal(proof(f), "dts_audiodec qtdemux matroskademux tsdemux");
  r = run(f, service.c2Disable(true, o));
  ok(r, "native uninstall"); assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.deepEqual(mounted(f), []);
  assert.equal(fs.existsSync(f.state), false, "uninstall removes the dedicated state");
  assert.equal(fs.existsSync(o.hook), false);
  assert.equal(fs.readFileSync(f.gstcool, "utf8"), "avdec_dca=0\n");
});

test("the legacy C2/G2 sets still bind libgstlibav and gstcool.conf with an avdec_dca proof", function () {
  [0, 1].forEach(function (i) {
    var f = legacyTv(i), o = f.overrides();
    var r = run(f, service.c2Enable(true, o));
    ok(r, "legacy enable " + i); assert.match(r.stdout, /^OK$/m, r.stdout);
    assert.deepEqual(mounted(f), demuxerTargets(f, true));
    assert.equal(proof(f), "avdec_dca qtdemux matroskademux tsdemux");
    assert.equal(fs.readFileSync(o.configSource, "utf8"), "avdec_dca=290\n");
    assert.equal(fs.existsSync(path.join(f.gst, "libgstlibav.so")), true);
    var kv = kvOf(run(f, service.c2StatusProbe(o)).stdout);
    assert.equal(kv.NATIVE, "0", "a legacy baseline is never native");
    assert.equal(service.c2StatusBindsComplete(kv), true);
    r = run(f, service.c2SelfTest(o));
    assert.match(r.stdout, /^mp4=PASS:\d+$/m, "legacy self-test still decodes");
    write(f.mountinfo, "1 0 0:1 / / rw - fake root rw\n");
    r = run(f, 'exec sh "' + o.init + '"');
    ok(r, "legacy boot " + i);
    assert.deepEqual(mounted(f), demuxerTargets(f, true));
    assert.equal(proof(f), "avdec_dca qtdemux matroskademux tsdemux");
  });
});
