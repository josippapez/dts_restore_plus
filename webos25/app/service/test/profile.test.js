"use strict";

var test = require("node:test").test;
var h = require("./helpers"), assert = h.assert, fs = h.fs, os = h.os, path = h.path, spawnSync = h.spawnSync;
var service = h.service, C = h.C, sh = h.sh, write = h.write, exe = h.exe, fixture = h.fixture, run = h.run, ok = h.ok;
var initFn = h.initFn, loadService = h.loadService, ROOT = h.ROOT, SERVICE = h.SERVICE;

/* A registered dts_audiodec must not displace an exact-matched C2 profile.
 *
 * Regression for issue #1: an OLED55CS6LA matched all eleven C2 gates, but its
 * gstcool.conf carries dts_audiodec=290 + avdec_dca=0, so the behavioral override
 * relabelled it native-dts-gated and the opt-in button was never rendered. On the
 * C2 family that element registers over a 128 KB stub with no decoder internals,
 * so the override must yield to the exact gate -- while still firing for C3/C4,
 * where the decoder is real and the demuxer gate is the actual problem. */
test("dts_audiodec override yields to an exact C2 match but still fires elsewhere", function () {
  var probe = service.DETECT_PROBE;
  var start = probe.indexOf("# --- PROFILE SELECTION ---");
  var end = probe.indexOf('echo "PROFILE=$PROFILE"');
  assert.ok(start > 0 && end > start, "selection block not found in DETECT_PROBE");
  var block = probe.slice(start, end);

  function profileFor(v) {
    var pre = [
      "set -u",
      "GST_VERSION=" + v.gst,
      'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
      "HARDWARE_ID=" + v.hw,
      "PRODUCT_ID=" + v.pid,
      "BOARD_TYPE=" + (v.board || "O22_DVB"),
      "WEBOS_RELEASE=" + (v.webos || "9.2.2"),
      "WEBOS_MANUFACTURING_VERSION=" + (v.fw || "23.25.55"),
      "LOADER=" + (v.loader || "ld-linux.so.3"),
      "FLOAT_ABI=" + (v.abi || "soft"),
      "HAS_DTS_AUDIODEC=" + v.audiodec,
      "DCA_RANK=" + v.rank
    ].join("\n");
    var r = sh(pre + "\n" + block + '\nprintf "%s" "$PROFILE"');
    assert.strictEqual(r.status, 0, "selection block failed: " + r.stderr);
    // The block also echoes C2_GATE_FAIL, so take only the final printf.
    var lines = r.stdout.trim().split("\n");
    return lines[lines.length - 1].trim();
  }

  // The exact-matched C2/CS set from issue #1: keeps its own profile.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    audiodec: "yes", rank: "0"
  }), C.C2, "an exact C2 match must not be relabelled native-dts-gated");

  // C3: decoder is real, demuxer gate off -> the override is correct here.
  assert.strictEqual(profileFor({
    gst: "1.18.4", hw: "HE_DTV_W23O_AFAAATAA", pid: "OLED55C34LA",
    audiodec: "yes", rank: "0"
  }), "native-dts-gated");

  // C3 with the rank already raised -> plain native.
  assert.strictEqual(profileFor({
    gst: "1.18.4", hw: "HE_DTV_W23O_AFAAATAA", pid: "OLED55C34LA",
    audiodec: "yes", rank: "290"
  }), "native-dts");

  // A C2-family set that does NOT match the exact gate keeps the diagnostic
  // profile rather than silently claiming native DTS.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    fw: "99.99.99", audiodec: "no", rank: "0"
  }), "webos22-o22-c2-diagnostic");

  // ...and it must survive the override too: the diagnostic profile is the only one
  // that reports WHICH gate missed, so relabelling it hides that from the very TVs
  // that need it.
  assert.strictEqual(profileFor({
    gst: "1.18.5", hw: "HE_DTV_W22O_AFABATPU", pid: "OLED55CS6LA",
    fw: "99.99.99", audiodec: "yes", rank: "0"
  }), "webos22-o22-c2-diagnostic",
    "a near-miss C2 must keep its diagnostic profile, not become native-dts-gated");
});

/* Issue #3: a C1 on GStreamer 1.16.2 was refused as unknown, while upstream
 * dts_restore 2.0 (byte-identical to payload/cx) works there. */
test("a soft-float GStreamer 1.16 C1 selects the CX-payload profile", function () {
  var probe = service.DETECT_PROBE;
  var block = probe.slice(probe.indexOf("# --- PROFILE SELECTION ---"), probe.indexOf('echo "PROFILE=$PROFILE"'));
  function profileFor(loader, abi) {
    var pre = [
      "set -u", "GST_VERSION=1.16.2", 'GST_MM=$(printf "%s" "$GST_VERSION" | cut -d. -f1-2)',
      "HARDWARE_ID=HE_DTV_W21O_AFABATAA", "PRODUCT_ID=OLED55C11LB", "BOARD_TYPE=O20_DVB",
      "WEBOS_RELEASE=6.5.0", "WEBOS_MANUFACTURING_VERSION=03.51.16",
      "LOADER=" + loader, "FLOAT_ABI=" + abi, "HAS_DTS_AUDIODEC=no", "DCA_RANK=0"
    ].join("\n");
    var r = sh(pre + "\n" + block + '\nprintf "%s" "$PROFILE"');
    assert.strictEqual(r.status, 0, "selection block failed: " + r.stderr);
    var lines = r.stdout.trim().split("\n");
    return lines[lines.length - 1].trim();
  }
  assert.strictEqual(profileFor("ld-linux.so.3", "soft"), "webos6-armel-gst116");
  assert.strictEqual(profileFor("ld-linux-armhf.so.3", "hard"), "unknown-gst1.16-ld-linux-armhf.so.3");
  assert.ok(service.isKnownProfile("webos6-armel-gst116"));
  assert.strictEqual(service.compatVerdict("webos6-armel-gst116", {}), null);
});

/* Issue #7: a UR7500 reports edidType "ac3", and the opt-in only knew "TrueHD", so it
 * changed nothing. A hand edit there also left "ac3+dts+truehd" in configd's cache. */
test("the DTS-tracks opt-in maps ac3 to ac3+dts and replaces any cached value", function () {
  assert.strictEqual(service.w25EdidTarget("TrueHD", true), "TrueHD+dts");
  assert.strictEqual(service.w25EdidTarget("ac3", true), "ac3+dts");
  assert.strictEqual(service.w25EdidTarget("ac3+dts+truehd", true), "ac3+dts");
  assert.strictEqual(service.w25EdidTarget("ac3+dts", false), "ac3");
  assert.strictEqual(service.w25EdidTarget("TrueHD+dts", false), "TrueHD");
  assert.strictEqual(service.w25EdidTarget("ac3", false), "ac3");

  var body = service.w25InitScriptBody().split("\n");
  var fn = body.slice(body.indexOf("w25_appdts_target() {"));
  fn = fn.slice(0, fn.indexOf("}") + 1);
  function line(re) { var l = body.filter(function (x) { return re.test(x); }); assert.strictEqual(l.length, 1, String(re)); return l[0]; }
  var getStock = line(/^\s*EDID_STOCK=\$\(grep/);
  var getDts = line(/^\s*EDID_DTS=\$\(w25_appdts_target/);
  var patchCache = line(/sed -i .*\[\^\\"\]\*.*\$EDID_DTS.*CONFIGD_DB/).replace(/^\s*if /, "").replace(/ &&\s*$/, "");
  // The TV's busybox sed takes GNU-style -i; BSD sed needs an explicit empty suffix.
  if (process.platform === "darwin") patchCache = patchCache.replace("sed -i ", "sed -i '' ");
  function run(stockValue, cached) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-edid-"));
    var lls = path.join(dir, "factorydb.json"), db = path.join(dir, "configd_db.json");
    write(lls, '{"tv.model":{"edidType":"' + stockValue + '","oledCPC":1}}');
    write(db, '{"tv.model": {"edidType" : "' + cached + '", "oledCPC": 1}}');
    var r = sh(["set -u", "LOG=/dev/null", "LLS=" + lls, "CONFIGD_DB=" + db].concat(fn, [getStock, getDts, patchCache, 'printf "%s" "$EDID_DTS"']).join("\n"));
    assert.strictEqual(r.status, 0, r.stderr);
    return { target: r.stdout, db: fs.readFileSync(db, "utf8") };
  }
  var ur = run("ac3", "ac3+dts+truehd");
  assert.strictEqual(ur.target, "ac3+dts");
  assert.strictEqual(ur.db, '{"tv.model": {"edidType" : "ac3+dts", "oledCPC": 1}}');
  assert.strictEqual(run("TrueHD", "TrueHD").target, "TrueHD+dts");
  assert.strictEqual(run("eac3", "eac3").target, "");
});

/* Issues #5 and #10: on webOS 11.2 (43.21.77) the driver logs its completions at
 * lines 3491/3727 instead of 3410/3644, and the persisted log starts 0.5s after
 * audiooutputd. The old check missed a held port on the first and refused every
 * boot on the second. */
test("the audio-idle check reads webOS 11.2 logs", function () {
  var fn = initFn("w25_appdts_audio_idle");
  function line(ts, fnName, n, text) {
    return "2026-10-25T20:21:34.148484Z [" + ts + "] kern.notice kernel alsasndout NOT (03107:03382) " +
      fnName + "           " + n + ": " + text;
  }
  var connect = [line("3.279607580", "pmlog", 1, "boot"),
    line("18.6", "sndout_connect", 3305, "Start Connect output 0x1 input 0x1 port 1."),
    line("18.7", "sndout_connect", 3491, "output 0x1 input 0x1 port 1 .")];
  var disconnect = [line("40.1", "sndout_disconnect", 3569, "Start Disconnect output 0x1 input 0x1 port 1 ."),
    line("40.2", "sndout_disconnect", 3727, "output 0x1 input 0x1 port 1 .")];
  function idle(lines, aodUs) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-idle-"));
    var log = path.join(dir, "legacy-log");
    write(log, lines.join("\n") + "\n");
    var r = sh(["systemctl() { echo " + aodUs + "; }", fn.split("/var/log/legacy-log").join(log), "w25_appdts_audio_idle"].join("\n"));
    return r.status === 0;
  }
  assert.strictEqual(idle(connect.concat(disconnect), 2777195), true, "freed port, log 0.5s after audiooutputd");
  assert.strictEqual(idle(connect, 2777195), false, "held port, log 0.5s after audiooutputd");
  assert.strictEqual(idle(connect, 3000000), false, "held port, 11.2 line numbers");
  assert.strictEqual(idle(connect.concat(disconnect), 279607), false, "log starts 3s after audiooutputd");
});

/* Issue #10: webOS 11 refuses the service's Homebrew Channel calls ("Message not
 * processed") until org.webosbrew.hbchannel.service.group is in its client permissions. */
test("the boot hook adds the Homebrew Channel group to the service's permissions", function () {
  var fn = initFn("w25_hb_permission");
  if (process.platform === "darwin") fn = fn.replace("sed -i ", "sed -i '' ");
  function run(content) {
    var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-perm-"));
    var f = path.join(dir, "perm.json");
    write(f, content);
    var r = sh(["w25_log() { :; }", "HB_GROUP=org.webosbrew.hbchannel.service.group", "HB_PERM=" + f, fn, "w25_hb_permission"].join("\n"));
    assert.strictEqual(r.status, 0, r.stderr);
    return fs.readFileSync(f, "utf8");
  }
  var added = '{"io.github.josippapez.dtsenabler.service*":["public","org.webosbrew.hbchannel.service.group"]}';
  assert.strictEqual(run('{"io.github.josippapez.dtsenabler.service*":["public"]}'), added);
  assert.strictEqual(run(added), added);
  var other = '{"io.github.josippapez.dtsenabler.service*":["public"],"other":["x"]}';
  assert.strictEqual(run(other), other);
});

/* Issue #13: the luna hub drops any message over 128 KiB and the app only sees
 * "Message not processed". 2.7.45's Enable script was 133,818 bytes, so Enable
 * failed on every TV. It must stay under the limit with room to grow, and the
 * boot script it writes must still come out byte for byte. */
test("the Enable call fits a luna message and writes the exact boot script", function () {
  var script = service.w25Enable(false);
  var size = Buffer.byteLength(JSON.stringify({ command: script }));
  assert.ok(size < 110 * 1024, "Enable exec payload is " + size + " bytes; the hub drops anything over 131072");
  var lines = script.split("\n");
  var start = lines.findIndex(function (l) { return /^base64 -d <<'B64EOF' \| gzip -dc > /.test(l); });
  assert.ok(start !== -1, "boot script heredoc not found");
  var end = lines.indexOf("B64EOF", start);
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-enable-"));
  var out = path.join(dir, "init.sh");
  var block = [lines[start].replace(/> "[^"]*"$/, '> "' + out + '"')].concat(lines.slice(start + 1, end + 1));
  var r = sh(block.join("\n"));
  assert.strictEqual(r.status, 0, r.stderr);
  assert.strictEqual(fs.readFileSync(out, "utf8"), service.w25InitScriptBody());
});

/* The generated script must resolve $APPBASE, not carry it as a literal.
 *
 * Regression for issue #1's actual cause: c2Q() single-quotes every config value,
 * which is correct for all of them except the payload path -- the one value that
 * deliberately contains a shell variable. Emitting C2_PAYLOAD='$APPBASE/payload/cx'
 * made the existence check test a path containing a dollar sign, so every C2 enable
 * failed as "payload file absent" and rolled back. No C2/G2 TV had ever completed
 * an enable because of it.
 *
 * Every other test builds the script with testOverrides, which substitutes a real
 * temp path and so never exercises the indirection. This one deliberately uses the
 * DEFAULT config. */
test("the default generated script expands $APPBASE in the payload path", function () {
  var script = service.c2Enable(true);

  var line = script.split("\n").filter(function (l) { return l.indexOf("C2_PAYLOAD=") === 0; })[0];
  assert.ok(line, "no C2_PAYLOAD assignment in the generated script");
  assert.ok(line.indexOf("'$APPBASE") === -1,
    "C2_PAYLOAD must not be single-quoted around $APPBASE: " + line);

  // Behavioral, not just textual: run the prelude plus the assignment and confirm
  // the value the script would actually use contains no unexpanded variable.
  var prelude = script.split("\n").slice(0, 30).filter(function (l) {
    return l.indexOf("APPBASE=") === 0 || l.indexOf("C2_PAYLOAD=") === 0;
  }).join("\n");
  var r = sh(prelude + '\nprintf "%s" "$C2_PAYLOAD"');
  assert.strictEqual(r.status, 0, "prelude failed: " + r.stderr);
  assert.ok(r.stdout.indexOf("$") === -1,
    "resolved payload path still contains a variable: " + r.stdout);
  assert.match(r.stdout, /\/payload\/cx$/);
});

/* A `refused` SHA verdict must say WHICH fault occurred.
 *
 * From issue #1: after an interrupted enable, every stock hash came back empty and
 * the app said only "SHA-256 is unavailable or returned an invalid digest". That one
 * sentence covers three different faults -- no hashing tool, unreadable targets, and
 * a malformed digest -- and because !hashable is evaluated before the foreign-bind
 * case it also masks a leftover bind. The owner had no way to act on it. */
test("the SHA refusal distinguishes no-tool, unreadable, and malformed", function () {
  var full = "a".repeat(64);

  var noTool = service.compatVerdict(C.C2, {C2_HASH_TOOL: "0", C2_GSTCOOL_SHA256: ""});
  assert.strictEqual(noTool.verdict, "refused");
  assert.match(noTool.verdictReason, /No working sha256sum/);

  // Tool present, targets unreadable: name them and point at the likely cause.
  var unreadable = service.compatVerdict(C.C2, {
    C2_HASH_TOOL: "1", C2_LIBAV_SHA256: "", C2_ISOMP4_SHA256: "",
    C2_MATROSKA_SHA256: "", C2_GSTCOOL_SHA256: ""
  });
  assert.strictEqual(unreadable.verdict, "refused");
  assert.strictEqual(unreadable.canForce, false);
  assert.match(unreadable.verdictReason, /Could not read these stock files/);
  assert.match(unreadable.verdictReason, /libgstlibav\.so/);
  assert.match(unreadable.verdictReason, /libgstmatroska\.so/);
  assert.match(unreadable.verdictReason, /reboot/i);

  // A digest that is present but not 64 hex chars is a different fault, and must
  // NOT be reported as an unreadable file.
  var malformed = service.compatVerdict(C.C2, {
    C2_HASH_TOOL: "1", C2_LIBAV_SHA256: full, C2_ISOMP4_SHA256: full,
    C2_MATROSKA_SHA256: full, C2_GSTCOOL_SHA256: "zz"
  });
  assert.strictEqual(malformed.verdict, "refused");
  assert.match(malformed.verdictReason, /invalid digest/);
  assert.ok(malformed.verdictReason.indexOf("Could not read") === -1,
    "a malformed digest must not be reported as an unreadable file");
});

/* Every generated C2 script must define $APPBASE before using it.
 *
 * All of them run under `set -u`, and only two of the six that embed the C2 vars
 * prepended APPBASE_PRELUDE. When C2_PAYLOAD started expanding $APPBASE (2.7.7),
 * DETECT_PROBE and the boot hook died on that assignment line and every line after
 * it silently never ran -- the C2 fields came back empty and the app refused with
 * "no working sha256sum". 2.7.7 was verified against the enable script only, which
 * was one of the two that happened to be correct. */
test("every generated C2 script defines $APPBASE before using it", function () {
  var scripts = {
    DETECT_PROBE: service.DETECT_PROBE,
    c2Enable: service.c2Enable(true),
    c2Disable: service.c2Disable(false),
    c2InitScriptBody: service.c2InitScriptBody(),
    c2StatusProbe: service.c2StatusProbe(),
    c2SelfTest: service.c2SelfTest()
  };
  Object.keys(scripts).forEach(function (name) {
    var lines = scripts[name].split("\n");
    var use = lines.findIndex(function (l) {
      return l.indexOf("$APPBASE") >= 0 && l.indexOf("APPBASE=/media") !== 0;
    });
    if (use === -1) return;                       // does not use it: nothing to prove
    var def = lines.findIndex(function (l) { return l.indexOf("APPBASE=") === 0; });
    assert.notStrictEqual(def, -1, name + " uses $APPBASE but never defines it");
    assert.ok(def < use,
      name + " defines APPBASE at line " + def + " but uses it at " + use);
  });

  // Behavioral: the probe must run to completion under `set -u` on a machine with
  // none of the TV's tools, and still emit its C2 fields. An unbound variable would
  // abort it mid-script with a zero exit from the last successful echo.
  var r = sh(service.DETECT_PROBE);
  assert.strictEqual(r.status, 0, "probe exited " + r.status + ": " + r.stderr);
  assert.ok(r.stderr.indexOf("unbound variable") === -1,
    "probe hit an unbound variable: " + r.stderr);
  ["C2_HASH_TOOL=", "C2_OWNED=", "C2_FOREIGN=", "C2_MOUNT_LIBAV="].forEach(function (k) {
    assert.ok(r.stdout.indexOf("\n" + k) >= 0 || r.stdout.indexOf(k) === 0,
      "probe did not emit " + k + " (it aborted before the C2 block)");
  });
});

/* Every self-test case the C2 shell can emit must be mapped by the handler.
 *
 * check-kv-sync.js covers the UPPER_SNAKE probe keys, but cannot judge the short
 * lowercase self-test keys (`mp4`, `ts`, `m2ts`) because those substrings appear all
 * over service.js and app.js. That is exactly the gap the 2.7.14 bug fell through:
 * the shell ran ts/m2ts and the handler's file map listed only mp4, so the results
 * were discarded and the UI showed "-" while all three clips actually played. */
test("the C2 self-test handler maps every case its shell can emit", function () {
  var shell = service.c2SelfTest();

  // Cases the shell can produce: the literal mp4 echo plus the `for c in ...` loop.
  var emitted = {};
  (shell.match(/echo\s+"([a-z0-9]+)=(?:PASS|FAIL)/g) || []).forEach(function (m) {
    emitted[m.replace(/echo\s+"/, "").replace(/=.*/, "")] = true;
  });
  var loop = shell.match(/for\s+c\s+in\s+([a-z0-9 ]+);\s*do/);
  if (loop) loop[1].trim().split(/\s+/).forEach(function (w) { emitted[w] = true; });

  assert.ok(emitted.mp4, "the self-test must still cover mp4");
  assert.ok(emitted.ts && emitted.m2ts,
    "the self-test shell should emit ts and m2ts; got " + Object.keys(emitted).join(","));

  // The handler's file map is the consumer. Read it out of the source rather than
  // duplicating the list here, so the test tracks the real mapping.
  var src = fs.readFileSync(SERVICE, "utf8");
  var mapLine = src.match(/var files = \{([^}]*)\};/);
  assert.ok(mapLine, "could not find the self-test file map in the handler");
  var mapped = {};
  (mapLine[1].match(/([a-z0-9]+)\s*:/g) || []).forEach(function (k) {
    mapped[k.replace(/\s*:$/, "")] = true;
  });

  Object.keys(emitted).forEach(function (k) {
    assert.ok(mapped[k],
      "the C2 self-test shell emits '" + k + "=' but the handler's file map does not " +
      "list it, so that result is silently discarded (this is the 2.7.14 bug)");
  });
});
