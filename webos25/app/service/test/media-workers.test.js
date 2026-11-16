"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var {ROOT, service, sh, write, ok} = require("./helpers");

var init = service.w25InitScriptBody();
var workerRefresh = init.match(/^w25_refresh_media_workers\(\) \([\s\S]*?^\)/m)[0];

test("worker refresh requires two idle snapshots and rechecks process identity", function (t) {
  var proc = fs.mkdtempSync(path.join(os.tmpdir(), "dts-media-workers-"));
  t.after(function () { fs.rmSync(proc, {recursive: true, force: true}); });

  var live = path.join(proc, "libgstisomp4.so");
  var mapped = path.join(proc, "42/map_files/1000-2000");
  var stat = "42 (media worker) S" + " 0".repeat(18);
  write(live, "current");
  write(mapped, "old");
  write(path.join(proc, "42/cmdline"), "/usr/sbin/starfish-media-pipeline\0pipeline_process\0");
  write(path.join(proc, "42/maps"), "1000-2000 r-xp 00000000 00:00 1 " + live + " (deleted)\n");

  function refresh(first, second, options) {
    write(path.join(proc, "42/stat"), stat + " 100\n");
    var result = sh([
      workerRefresh,
      'LGISO="$PROC/libgstisomp4.so"; MYISO=$LGISO',
      'w25_bound() { echo 1; }',
      'w25_md5() { [ -f "$1" ] && cat "$1"; }',
      'pidof() { echo 42; }',
      'timeout() {',
      '  if [ "$w25_pool_check" = 1 ]; then printf "%s" "$FIRST"; return; fi',
      '  [ "$REUSED" = 0 ] || printf "%s 101\n" "$STAT" > "$PROC/42/stat"',
      '  [ "$TIMEOUT" = 0 ] || return 124',
      '  printf "%s" "$SECOND"',
      '}',
      'w25_log() { :; }',
      'kill() { printf "%s\n" "$*"; }',
      'w25_refresh_media_workers "$PROC"'
    ].join("\n"), Object.assign({
      PROC: proc, STAT: stat, FIRST: first, SECOND: second,
      REUSED: "0", TIMEOUT: "0", W25_REFRESH_MEDIA: "0"
    }, options));
    ok(result, "refresh media workers");
    return result.stdout.trim();
  }

  assert.equal(refresh("[]", "[ ]"), "-TERM 42");
  assert.equal(refresh("[]", '[{"pid":42}]'), "");
  assert.equal(refresh("[{}]", "[]"), "");
  assert.equal(refresh("[]", "[]", {TIMEOUT: "1"}), "");
  assert.equal(refresh("[]", "[]", {REUSED: "1"}), "");
  write(mapped, "current");
  assert.equal(refresh("[]", "[]"), "");
  assert.equal(refresh("[]", "[]", {W25_REFRESH_MEDIA: "1"}), "-TERM 42");
});

test("Enable delegates worker refresh to the registry commit", function () {
  var enable = service.w25Enable(false);
  var install = fs.readFileSync(path.join(ROOT, "webos25/restore/install.sh"), "utf8");
  for (var script of [enable, install]) {
    assert.match(script, /APPLY=\$\(W25_REFRESH_MEDIA=1 /);
    assert.doesNotMatch(script, /killall starfish-media-pipeline/);
  }
  assert.match(init, /if cp -f "\$REG_TMP" "\$REG" [^\n]*\n[^\n]*\n    w25_refresh_media_workers/);
});
