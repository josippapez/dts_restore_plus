"use strict";

var test = require("node:test").test;
var h = require("./helpers"), assert = h.assert, fs = h.fs, os = h.os, path = h.path, spawnSync = h.spawnSync;
var service = h.service, C = h.C, sh = h.sh, write = h.write, exe = h.exe, fixture = h.fixture, run = h.run, ok = h.ok;
var initFn = h.initFn, loadService = h.loadService, ROOT = h.ROOT, SERVICE = h.SERVICE;

/* The optional TS demuxer must never cost a working install.
 *
 * The C2 profile works today with four 1.14.4 binaries and MP4/MKV. Adding a fifth
 * (LG 1.14.4 libgstmpegtsdemux.so) introduces two NEEDED libraries whose presence on
 * a C2/G2/CS is unverified. c2_payload requires its mandatory files to copy AND trace,
 * so a hard fifth file would fail the whole enable and take away working MP4 on any
 * TV lacking those libs. It must degrade instead. */
test("a failing TS demuxer degrades to MP4/MKV instead of failing the enable", function () {
  var TSF = "libgstmpegtsdemux.so";

  // 1. TS not shipped at all (an older .ipk, or the payload dir absent).
  var f = fixture(), o = f.overrides();
  var r = run(f, service.c2Enable(true, o));
  ok(r, "enable with no TS payload");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
  assert.equal(fs.existsSync(path.join(f.state, "owner")), true,
    "the working four must still produce an owned install");

  // 2. TS shipped, stock target present, but its dependencies do not resolve here.
  //    This is the exact risk carried in DESIGN-C2-TS-DEMUX.md.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  write(path.join(f.target, TSF), "stock-" + TSF);
  r = run(f, service.c2Enable(true, o), {TS_MISS_DEP: "1"});
  ok(r, "enable with an unresolvable TS demuxer");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
  assert.match(r.stdout, /TS_SKIP=/);
  assert.equal(fs.existsSync(path.join(f.state, "owner")), true,
    "a bad TS demuxer must not roll back the working install");
  assert.equal(fs.existsSync(path.join(f.gst, TSF)), false,
    "the unusable TS file must not be left staged");
  assert.equal(fs.existsSync(path.join(f.state, "trace." + TSF)), true);
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall with only the skipped TS trace left");
  assert.match(r.stdout, /^OK$/m);
  assert.equal(fs.existsSync(f.state), false);

  // 3. TS shipped, traces clean, stock target present -> bound.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  write(path.join(f.target, TSF), "stock-" + TSF);
  r = run(f, service.c2Enable(true, o));
  ok(r, "enable with a usable TS demuxer");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=1/);
  // #22: the staged TS demuxer and its trace are ours, so Uninstall must remove
  // them rather than refuse the whole state as unexpected.
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall with the TS demuxer staged");
  assert.doesNotMatch(r.stdout, /REFUSED/, r.stdout);
  assert.equal(fs.existsSync(f.state), false, "uninstall must remove the dedicated C2 state");

  // 4. TS shipped and usable, but the TV has no stock TS demuxer to override.
  f = fixture(); o = f.overrides();
  write(path.join(f.payloadts, TSF), "payload-" + TSF);
  r = run(f, service.c2Enable(true, o));
  ok(r, "enable with no stock TS target");
  assert.match(r.stdout, /VERDICT=forced/);
  assert.match(r.stdout, /TS_BOUND=0/);
});

test("C2 uninstall after a firmware upgrade leaves the new rootfs and webOS 25 state alone", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  function firmware(fw, webos, gst, hashes) {
    exe(f.command("nyx-cmd"), 'case "$*" in *hardware_id) echo HE_DTV_W22O_AFABATPU;; *product_id) echo OLED55CS6LA;; *board_type) echo O22_DVB;; *webos_manufacturing_version) echo ' + fw + ';; *webos_release) echo ' + webos + ';; esac');
    exe(f.command("gst-inspect-1.0"), '[ "$1" = --version ] && { echo "GStreamer ' + gst + '"; exit 0; }; if [ -n "${GST_REGISTRY_1_0:-}" ] && [ "$#" -eq 0 ]; then printf registry > "$GST_REGISTRY_1_0" || exit 1; fi; exit 0');
    exe(f.command("sha256sum"), 'case "$1" in *libgstlibav.so) h="' + hashes.libav + '";; *libgstisomp4.so) h="' + hashes.iso + '";; *libgstmatroska.so) h="' + hashes.mkv + '";; *gstcool.conf) h="' + "a".repeat(64) + '";; *) exec /usr/bin/shasum -a 256 "$1";; esac; echo "$h  $1"');
  }
  firmware("23.25.55", "9.2.2", "1.18.5", C.sets[1]);
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare the pre-upgrade C2 install");
  assert.match(r.stdout, /TS_BOUND=1/);
  assert.match(fs.readFileSync(o.baseline, "utf8"), /firmware=23\.25\.55\n/);

  write(f.mountinfo, "1 0 0:1 / / rw - fake root rw\n");
  fs.unlinkSync(path.join(f.target, "libgstisomp4_1_8.so"));
  var untouched = [f.gstcool, f.registry];
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", ts].forEach(function (name) {
    var file = path.join(f.target, name);
    write(file, "upgraded-stock-" + name);
    untouched.push(file);
  });
  ["dts25/libgstdtsdec.so", "truehd/libgstlibav.so", "demux25/libgstisomp4.so", "init.d/restore_dts25"].forEach(function (name) {
    var file = path.join(f.dir, name);
    write(file, "webos25-" + name);
    untouched.push(file);
  });
  var before = untouched.map(function (file) { return fs.readFileSync(file, "utf8"); });
  firmware("33.31.68", "10.3.1", "1.24.0", {
    libav: "061df7c4d219f3a9e03b4888ee09e9e10fda79b2ce40f916d14d39a225af2697",
    iso: "8450bd3efe8055d1f37875fab33e489ce268a60c5e13aa907da9c283dec16d62",
    mkv: "986bb059463d40f8d5581289a390a4576786bf2d339a0a61dc94482b5d426209"
  });
  assert.equal(service.c2OwnerRoute("enable", "webos25-armel-gst124", {verdict: "drift"}), "refuse");
  assert.equal(service.c2OwnerRoute("uninstall", "webos25-armel-gst124", {verdict: "drift"}), "uninstall");
  r = run(f, 'exec sh "' + o.init + '"');
  ok(r, "boot after the firmware upgrade");
  assert.match(r.stdout, /REFUSED=recovery/);
  assert.match(r.stdout, /persisted identity or hashes drifted/);
  assert.equal(fs.readFileSync(f.mountinfo, "utf8"), "1 0 0:1 / / rw - fake root rw\n");
  r = run(f, service.c2Disable(true, o));
  ok(r, "uninstall the stale C2 install");
  assert.match(r.stdout, /^OK$/m, r.stdout);
  assert.equal(fs.existsSync(f.state), false);
  assert.equal(fs.existsSync(o.hook), false);
  untouched.forEach(function (file, i) { assert.equal(fs.readFileSync(file, "utf8"), before[i], file); });
});

test("C2 uninstall rejects TS symlinks and directories before mutation", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare TS state safety checks");
  assert.match(r.stdout, /TS_BOUND=1/);
  var before = fs.readFileSync(f.mountinfo, "utf8"), sentinel = path.join(f.dir, "unrelated");
  write(sentinel, "untouched");
  [path.join(f.gst, ts), path.join(f.state, "trace." + ts)].forEach(function (file) {
    var content = fs.readFileSync(file);
    ["symlink", "dangling symlink", "directory"].forEach(function (kind) {
      fs.unlinkSync(file);
      if (kind === "directory") fs.mkdirSync(file);
      else fs.symlinkSync(kind === "symlink" ? sentinel : sentinel + ".missing", file);
      r = run(f, service.c2Disable(true, o));
      ok(r, kind + " at " + file);
      assert.match(r.stdout, /unexpected entries or symlinks/);
      assert.equal(fs.readFileSync(f.mountinfo, "utf8"), before);
      assert.equal(fs.existsSync(o.recovery), false);
      assert.equal(fs.existsSync(o.owner), true);
      assert.equal(fs.existsSync(o.hook), true);
      assert.equal(fs.readFileSync(sentinel, "utf8"), "untouched");
      if (kind === "directory") fs.rmdirSync(file); else fs.unlinkSync(file);
      write(file, content);
    });
  });
});

test("C2 TS cleanup failures retain recovery state and permit a checked retry", function () {
  var f = fixture(), o = f.overrides(), ts = "libgstmpegtsdemux.so";
  write(path.join(f.payloadts, ts), "payload-" + ts);
  write(path.join(f.target, ts), "stock-" + ts);
  var r = run(f, service.c2Enable(true, o));
  ok(r, "prepare TS cleanup failures");
  assert.match(r.stdout, /TS_BOUND=1/);
  [path.join(f.gst, ts), path.join(f.state, "trace." + ts)].forEach(function (file) {
    r = run(f, service.c2Disable(true, o), {FAIL_RM_PATH: file});
    ok(r, "refuse failed cleanup of " + file);
    assert.match(r.stdout, /REFUSED=recovery/);
    assert.match(r.stdout, /checked C2 cleanup failed/);
    assert.doesNotMatch(r.stdout, /^OK$/m);
    [o.owner, o.baseline, o.recovery, o.init, o.env, o.hook].forEach(function (kept) {
      assert.equal(fs.existsSync(kept), true, kept);
    });
  });
  r = run(f, service.c2Disable(true, o));
  ok(r, "retry TS cleanup");
  assert.match(r.stdout, /^OK$/m);
  assert.equal(fs.existsSync(f.state), false);
  assert.equal(fs.existsSync(o.hook), false);
});
