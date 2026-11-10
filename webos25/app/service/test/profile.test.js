"use strict";

var test = require("node:test").test;
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var spawnSync = require("node:child_process").spawnSync;
var {service, C, sh, write, run, ok, initFn, SERVICE} = require("./helpers");

/* A registered dts_audiodec must not displace an exact-matched C2 profile.
 *
 * Regression for issue #1: an OLED55CS6LA matched all eleven C2 gates, but its
 * gstcool.conf carries dts_audiodec=290 + avdec_dca=0, so the behavioral override
 * relabelled it native-dts-gated and the opt-in button was never rendered. On the
 * C2 family that element registers over a 128 KB stub with no decoder internals,
 * so the override must yield to the exact gate -- while still firing for C3/C4,
 * where the decoder is real and the demuxer gate is the actual problem. */
test("native DTS detection preserves the webOS 25 and C2 profiles", function () {
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

  // The updated G3 keeps the webOS 25 restore path even with the stock DTS element
  // registered. The separate compatibility check decides whether Enable can run.
  assert.strictEqual(profileFor({
    gst: "1.24.0", hw: "HE_DTV_W23O_AFABATAA", pid: "OLED77G36LA",
    webos: "10.3.1", fw: "33.31.68", audiodec: "yes", rank: "0"
  }), "webos25-armel-gst124");

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

/* Show DTS tracks in apps sets tv.model.edidType live through configd's
 * setConfigs as root, the way the G5 project does: on a C5 nothing restarted and
 * nothing was written to disk, where the old bind + configd restart could leave
 * the TV without sound, volume or Home keys. Issue #7: a UR7500 reports "ac3". */
function appDtsShell(stock, live, extra) {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-edid-"));
  var lls = path.join(dir, "factorydb.json"), state = path.join(dir, "live"), calls = path.join(dir, "calls");
  var user = path.join(dir, "configd_factory_db.json");
  write(lls, '{"tv.model":{"edidType":"' + stock + '","oledCPC":1}}');
  write(state, live);
  write(path.join(dir, "flag"), "");
  var eimEnv = path.join(dir, "eim.env"), hooked = path.join(dir, "hooked"), myEdid = path.join(dir, "libedidaudio.so");
  write(myEdid, "");
  write(path.join(dir, "eim.service"), "[Service]\nEnvironment=LD_PRELOAD=$LD_PRELOAD:/usr/lib/libsystrim.so.3\nExecStart=/usr/sbin/eim\n");
  var body = service.w25InitScriptBody().split("\n");
  function oneLine(name) {
    var l = body.filter(function (x) { return x.indexOf(name + "() { ") === 0; });
    assert.strictEqual(l.length, 1, name);
    return l[0];
  }
  function assignment(name) {
    var l = body.filter(function (x) { return x.indexOf(name + "=") === 0; });
    assert.strictEqual(l.length, 1, name);
    return l[0];
  }
  return {
    calls: calls, state: state, eimEnv: eimEnv, hooked: hooked, myEdid: myEdid,
    run: function (cmd) {
      var r = sh([
        "set -u", "LOG=/dev/null", "LLS=" + lls, "MYLLS=" + path.join(dir, "override"),
        "APPDTS_FLAG=" + path.join(dir, "flag"), "APPDTS_LAST=" + path.join(dir, "last"),
        "APPDTS_HIST=" + path.join(dir, "hist"), "CFG_USER=" + user,
        "EIM_ENV=" + eimEnv, "EIM_UNIT=" + path.join(dir, "eim.service"), "MYEDID=" + myEdid,
        "EDID_STATE=" + path.join(dir, "edid-state"),
        "w25_log() { :; }", "toast() { :; }", "w25_umount() { :; }", "sleep() { :; }",
        // eim loads the hook only when its env file names it, and the hook then
        // writes its verdict ($VERDICT, a remap by default).
        "w25_eim_hooked() { [ -f " + hooked + " ]; }",
        "systemctl() {",
        "  echo \"systemctl $*\" >> " + calls,
        "  if grep -q libedidaudio \"$EIM_ENV\" 2>/dev/null; then : > " + hooked + "; echo \"${VERDICT:-remap TrueHD+dts=4->3 ac3+dts=2->1}\" > \"$EDID_STATE\"",
        "  else rm -f " + hooked + "; fi",
        "}",
        // A pty-backed luna-send: replies end in CRLF.
        "script() {",
        "  case \"$4\" in",
        "    *getConfigs*) printf '{\"subscribed\":false,\"configs\":{\"tv.model.edidType\":\"%s\"},\"returnValue\":true}\\r\\n' \"$(cat " + state + ")\" ;;",
        "    *setConfigs*) printf '%s' \"$4\" | sed 's/.*\"tv.model.edidType\":\"\\([^\"]*\\)\".*/\\1/' > " + state + "; echo set >> " + calls + ";",
        "      [ -n \"${KEEP_USER:-}\" ] || printf '{\\n    \"com.webos.service.config\": {\\n        \"usrDefined\": [\\n            \"tv.model.edidType\"\\n        ]\\n    },\\n    \"tv.model\": {\\n        \"edidType\": \"%s\"\\n    }\\n}\\n' \"$(cat " + state + ")\" > " + user + ";",
        "      printf '{\"returnValue\":true}\\r\\n' ;;",
        "  esac",
        "}",
        oneLine("w25_luna"), initFn("w25_edid_get"), initFn("w25_edid_set"), oneLine("w25_edid_stock"),
        initFn("w25_hdmi_hook_on"), initFn("w25_hdmi_hook_off"), assignment("HDMI_NOTE"),
        initFn("w25_appdts_report"), initFn("w25_appdts_target"), initFn("w25_appdts_apply"), initFn("w25_appdts_off"),
        extra || "", cmd, "cat " + path.join(dir, "last") + " 2>/dev/null; exit 0"
      ].join("\n"));
      assert.strictEqual(r.status, 0, r.stderr);
      return r.stdout;
    },
    live: function () { return fs.readFileSync(state, "utf8"); },
    user: user,
    log: function () { return fs.existsSync(calls) ? fs.readFileSync(calls, "utf8").split("\n").filter(Boolean) : []; },
    sets: function () { return this.log().filter(function (l) { return l === "set"; }).length; }
  };
}

/* With the opt-in on, eim built the HDMI-input EDIDs from "+dts" too, and a
 * streamer sent DTS the TV can't play from HDMI (silent). The hook keeps them
 * stock; it must be in eim before edidType gains "+dts" and stay until it loses it. */
test("the DTS-tracks opt-in keeps DTS off the HDMI inputs", function () {
  var t = appDtsShell("TrueHD", "TrueHD");
  var out = t.run("w25_appdts_apply");
  assert.deepStrictEqual(t.log(), ["systemctl restart eim", "set"], "hook first, then edidType");
  assert.strictEqual(fs.readFileSync(t.eimEnv, "utf8"), "LD_PRELOAD=" + t.myEdid + ":/usr/lib/libsystrim.so.3\n");
  assert.doesNotMatch(out, /HDMI devices/);
  // Boot: eim already loaded it from the env file, so it is not restarted.
  t = appDtsShell("TrueHD", "TrueHD+dts");
  write(t.eimEnv, "LD_PRELOAD=" + t.myEdid + "\n");
  write(t.hooked, "");
  t.run("echo 'remap TrueHD+dts=4->3 ac3+dts=2->1' > \"$EDID_STATE\"; w25_appdts_apply");
  assert.deepStrictEqual(t.log(), []);
  // A TV that ships with dts plays it from HDMI: its inputs keep it.
  t = appDtsShell("TrueHD+dts", "TrueHD+dts");
  assert.match(t.run("w25_appdts_apply"), / on DTS tracks are on for apps\.$/m);
  assert.deepStrictEqual(t.log(), []);
  assert.strictEqual(fs.existsSync(t.eimEnv), false);
  // A build whose table the hook does not recognise still turns DTS on, and says so.
  t = appDtsShell("TrueHD", "TrueHD");
  assert.match(t.run("VERDICT='off: table not recognised' w25_appdts_apply"), / on DTS tracks are on for apps\. .*HDMI devices are still told/);
  assert.strictEqual(t.live(), "TrueHD+dts");
  // No hook staged, or someone else's env file: eim is left alone.
  t = appDtsShell("TrueHD", "TrueHD");
  fs.unlinkSync(t.myEdid);
  assert.match(t.run("w25_appdts_apply"), /HDMI devices are still told/);
  assert.deepStrictEqual(t.log(), ["set"]);
  t = appDtsShell("TrueHD", "TrueHD");
  write(t.eimEnv, "FOO=1\n");
  t.run("w25_appdts_apply");
  assert.strictEqual(fs.readFileSync(t.eimEnv, "utf8"), "FOO=1\n");
  assert.deepStrictEqual(t.log(), ["set"]);
});

test("turning the DTS-tracks opt-in off resets edidType before eim drops the hook", function () {
  var t = appDtsShell("TrueHD", "TrueHD+dts");
  write(t.eimEnv, "LD_PRELOAD=" + t.myEdid + "\n");
  write(t.hooked, "");
  t.run("w25_appdts_off");
  assert.deepStrictEqual(t.log(), ["set", "systemctl restart eim"]);
  assert.strictEqual(fs.existsSync(t.eimEnv), false);
  // Someone else's env file stays.
  t = appDtsShell("TrueHD", "TrueHD+dts");
  write(t.eimEnv, "FOO=1\n");
  t.run("w25_appdts_off");
  assert.strictEqual(fs.readFileSync(t.eimEnv, "utf8"), "FOO=1\n");
  assert.deepStrictEqual(t.log(), ["set"]);
});

test("the DTS-tracks opt-in sets the live value with setConfigs and reads it back", function () {
  var t = appDtsShell("TrueHD", "TrueHD");
  assert.match(t.run("w25_appdts_apply"), / on DTS tracks are on for apps\. Reopen/);
  assert.strictEqual(t.live(), "TrueHD+dts");
  t = appDtsShell("ac3", "ac3");
  t.run("w25_appdts_apply");
  assert.strictEqual(t.live(), "ac3+dts");
  // Already on: nothing is written.
  t = appDtsShell("TrueHD", "TrueHD+dts");
  assert.match(t.run("w25_appdts_apply"), / on DTS tracks are on for apps\.$/m);
  assert.strictEqual(t.sets(), 0);
  // No known DTS form: nothing is written, and the reason is reported.
  t = appDtsShell("eac3", "eac3");
  assert.match(t.run("w25_appdts_apply"), / failed .*'eac3'/);
  assert.strictEqual(t.sets(), 0);
  // Without the marker the boot hook does nothing.
  t = appDtsShell("TrueHD", "TrueHD");
  t.run("rm -f \"$APPDTS_FLAG\"; w25_appdts_apply");
  assert.strictEqual(t.live(), "TrueHD");
});

test("turning the DTS-tracks opt-in off puts the factory value back and drops configd's override", function () {
  var t = appDtsShell("TrueHD", "TrueHD+dts");
  t.run("w25_appdts_off");
  assert.strictEqual(t.live(), "TrueHD");
  assert.strictEqual(fs.existsSync(t.user), false, "the override held only our key");
  // Someone else's overrides stay.
  t = appDtsShell("TrueHD", "TrueHD+dts");
  var other = '{"com.webos.service.config":{"usrDefined":["tv.model.edidType","tv.hw.x"]},"tv.model":{"edidType":"TrueHD"}}';
  write(t.user, other);
  t.run("KEEP_USER=1 w25_appdts_off");
  assert.strictEqual(fs.readFileSync(t.user, "utf8"), other);
  t = appDtsShell("ac3", "ac3");
  t.run("w25_appdts_off");
  assert.strictEqual(t.sets(), 0, "already stock");
});

test("the DTS-tracks toggle installs the current boot script before using its mode", function () {
  [true, false].forEach(function (on) {
    var script = service.w25AppDtsSteps(on);
    assert.strictEqual(spawnSync("sh", ["-n"], { input: script, encoding: "utf8" }).status, 0);
    assert.ok(Buffer.byteLength(JSON.stringify({ command: script })) < 110 * 1024, "fits a luna message");
    var install = script.indexOf("B64EOF\nchmod 0755");
    var mode = script.indexOf("W25_APPDTS=" + (on ? "on" : "off") + " sh ");
    assert.ok(install !== -1 && mode > install, "mode runs after the current script is written");
  });
  // Uninstall puts the factory value back while the script still exists.
  var u = service.w25Uninstall();
  assert.ok(u.indexOf("W25_APPDTS=off sh") !== -1 && u.indexOf("W25_APPDTS=off sh") < u.indexOf('rm -rf "'), "uninstall reverts before deleting");
  var body = service.w25InitScriptBody();
  var mode = body.indexOf('if [ -n "${W25_APPDTS:-}" ]; then');
  assert.ok(mode !== -1 && mode < body.indexOf("# --- G) self-heal"), "W25_APPDTS mode exits before the apply path");
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
