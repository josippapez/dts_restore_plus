"use strict";

/* Shared fixtures for the service tests. The service is loaded in a VM (webos-service
 * is a TV-only module); transactions run as real /bin/sh processes against temp paths. */
var assert = require("node:assert/strict");
var fs = require("node:fs");
var os = require("node:os");
var path = require("node:path");
var vm = require("node:vm");
var spawnSync = require("node:child_process").spawnSync;

var ROOT = path.resolve(__dirname, "..", "..", "..", "..");
var SERVICE = path.join(ROOT, "webos25/app/service/service.js");

function loadService(mocks) {
  var source = fs.readFileSync(SERVICE, "utf8") + "\nmodule.exports = {" +
    "compatVerdict: compatVerdict, isKnownProfile: isKnownProfile," +
    "c2OwnerRoute: c2OwnerRoute," +
    "c2Config: c2Config, c2InitScriptBody: c2InitScriptBody," +
    "c2Enable: c2Enable, c2Disable: c2Disable, c2StatusProbe: c2StatusProbe," +
    "c2StatusBindsComplete: c2StatusBindsComplete, c2SelfTest: c2SelfTest," +
    "DETECT_PROBE: DETECT_PROBE, w25AppDtsSteps: w25AppDtsSteps, w25InitScriptBody: w25InitScriptBody," +
    "w25PtReceiver: w25PtReceiver, w25HdOutput: w25HdOutput, w25PassthroughSteps: w25PassthroughSteps, w25Enable: w25Enable," +
    "w25HdReadinessSteps: w25HdReadinessSteps, w25HdReason: w25HdReason, w25RouteLogSteps: w25RouteLogSteps," +
    "w25Disable: w25Disable, w25Uninstall: w25Uninstall," +
    "w25KmodReason: w25KmodReason," +
    "constants: {C2: PROFILE_C2," +
    "sets: C2_EXPECTED_SETS, matchSet: c2MatchSet," +
    "libav: C2_EXPECTED_SETS[0].libav, iso: C2_EXPECTED_SETS[0].iso, mkv: C2_EXPECTED_SETS[0].mkv}};";
  var handlers = {};
  var FakeService = function () { this.register = function (name, handler) { handlers[name] = handler; }; };
  var context = {
    require: function (name) { return name === "webos-service" ? FakeService : require(name); },
    module: {exports: {}}, exports: {}, Buffer: Buffer, console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout
  };
  vm.runInNewContext(source, context, {filename: SERVICE});
  Object.keys(mocks || {}).forEach(function (name) { context[name] = mocks[name]; });
  context.module.exports.handlers = handlers;
  return context.module.exports;
}

var service = loadService();
var C = service.constants;

function sh(script, env) {
  var e = Object.assign({}, process.env, env || {});
  return spawnSync("sh", ["-c", script], {encoding: "utf8", env: e, timeout: 30000});
}
function write(file, text, mode) {
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.writeFileSync(file, text || "");
  if (mode) fs.chmodSync(file, mode);
}
// macOS assesses each new executable on first run (~100 ms); hard links to an already-run inode skip that.
var stubs = {};
function exe(file, text) {
  var body = "#!/bin/sh\n" + text + "\n";
  fs.mkdirSync(path.dirname(file), {recursive: true});
  fs.rmSync(file, {force: true});
  try { fs.linkSync(stubs[body], file); } catch (e) { write(file, body, 0o755); stubs[body] = file; }
}

/* A fixture contains only temporary paths.  Fake commands intentionally fail
 * through environment flags, so each failure is deterministic and observable. */
function fixture() {
  var dir = fs.mkdtempSync(path.join(os.tmpdir(), "dts-c2-"));
  var bin = path.join(dir, "bin");
  var state = path.join(dir, "state"), gst = path.join(state, "gst");
  var target = path.join(dir, "target"), payload = path.join(dir, "payload");
  var mountinfo = path.join(dir, "mountinfo"), registry = path.join(dir, "registry.bin");
  var payloadts = path.join(dir, "payload-ts");
  fs.mkdirSync(payloadts, {recursive: true});
  fs.mkdirSync(bin, {recursive: true});
  [target, payload, path.join(dir, "etc")].forEach(function (p) { fs.mkdirSync(p, {recursive: true}); });
  ["libgstlibav.so", "libgstisomp4.so", "libgstmatroska.so", "libgstisomp4_1_8.so"].forEach(function (f) {
    write(path.join(payload, f), "payload-" + f);
    write(path.join(target, f), "stock-" + f);
  });
  write(path.join(target, "libgstcoreelements.so"), "\0".repeat(36) + "\0\x02\0\0");
  write(path.join(dir, "etc/gstcool.conf"), "avdec_dca=0\n");
  write(registry, "stock-registry");
  write(mountinfo, "1 0 0:1 / / rw - fake root rw\n");
  exe(path.join(bin, "nyx-cmd"), 'case "$*" in *hardware_id) echo HE_DTV_W22O_AFABATAA;; *product_id) echo OLED65C2PUA;; *board_type) echo o22;; *webos_manufacturing_version) echo 04.40.93.01;; *webos_release) echo 7.4.0;; esac');
  exe(path.join(bin, "sha256sum"), '[ "${FAIL_HASH:-}" = 1 ] && exit 1; case "$1" in *libgstlibav.so) h="' + C.libav + '";; *libgstisomp4.so) h="' + C.iso + '";; *libgstmatroska.so) h="' + C.mkv + '";; *gstcool.conf) h="' + "a".repeat(64) + '";; *) exec /usr/bin/shasum -a 256 "$1";; esac; echo "$h  $1"');
  exe(path.join(bin, "gst-inspect-1.0"), '[ "$1" = --version ] && { echo "GStreamer 1.18.2"; exit 0; }; [ "${FAIL_INSPECT:-}" = 1 ] && exit 1; if [ -n "${GST_REGISTRY_1_0:-}" ] && [ "$#" -eq 0 ]; then printf registry > "$GST_REGISTRY_1_0" || exit 1; fi; exit 0');
  exe(path.join(bin, "gst-launch-1.0"), '[ "${FAIL_LAUNCH:-}" = 1 ] && exit 1; out=; for a in "$@"; do case "$a" in location=*) out=${a#location=};; esac; done; [ -n "$out" ] || exit 1; dd if=/dev/zero of="$out" bs=100001 count=1 >/dev/null 2>&1');
  exe(path.join(bin, "timeout"), 'shift; exec "$@"');
  exe(path.join(bin, "stat"), '[ "$1" = -c%s ] && exec /usr/bin/stat -f%z "$2"; exec /usr/bin/stat "$@"');
  // MISS_DEP reproduces the real shape of an unresolved dependency: the loader
  // SUCCEEDS and reports the missing library in its trace output. FAIL_TRACE is the
  // different case where the loader itself cannot run.
  exe(path.join(bin, "loader"), '[ "${FAIL_TRACE:-}" = 1 ] && { echo "not found"; exit 1; }; ' +
    '[ "${MISS_DEP:-}" = 1 ] && { echo "\tlibgstsubtitle-1.0.so.0 => not found"; exit 0; }; ' +
    // TS_MISS_DEP fails the trace for the TS demuxer ONLY: that case must degrade
    // to MP4/MKV, never fail the whole enable.
    'case "${1:-}" in *libgstmpegtsdemux.so) [ "${TS_MISS_DEP:-}" = 1 ] && { echo "\tlibgstmpegts-1.0.so.0 => not found"; exit 0; };; esac; exit 0');
  [["cp", "/bin/cp"], ["rm", "/bin/rm"], ["rmdir", "/bin/rmdir"], ["mkdir", "/bin/mkdir"], ["mv", "/bin/mv"], ["chmod", "/bin/chmod"], ["ln", "/bin/ln"], ["sed", "/usr/bin/sed"]].forEach(function (entry) {
    exe(path.join(bin, entry[0]), '[ "${FAIL_' + entry[0].toUpperCase() + ':-}" = 1 ] && exit 1; [ -n "${FAIL_' + entry[0].toUpperCase() + '_PATH:-}" ] && { for a in "$@"; do [ "$a" = "$FAIL_' + entry[0].toUpperCase() + '_PATH" ] && exit 1; done; }; exec ' + entry[1] + ' "$@"');
  });
  exe(path.join(bin, "mount"), 'if [ "${FAIL_MOUNT:-}" = 1 ]; then case "${INJECT_STATE_ENTRY:-}" in file) printf foreign > "' + state + '/foreign-entry";; hidden) printf foreign > "' + state + '/.foreign-hidden";; dir) /bin/mkdir "' + state + '/foreign-dir";; symlink) /bin/ln -s "' + dir + '" "' + state + '/foreign-link";; esac; exit 1; fi; target=""; source=""; for a in "$@"; do source="$target"; target="$a"; done; printf "2 1 0:1 %s %s rw - fake fake rw\n" "$source" "$target" >> "' + mountinfo + '"');
  exe(path.join(bin, "umount"), '[ "${FAIL_UMOUNT:-}" = 1 ] && exit 1; target=""; for a in "$@"; do target="$a"; done; tmp="' + mountinfo + '.tmp"; grep -v " $target " "' + mountinfo + '" > "$tmp"; mv "$tmp" "' + mountinfo + '"');
  return {dir: dir, bin: bin, state: state, gst: gst, target: target, payload: payload, payloadts: payloadts,
    mountinfo: mountinfo, registry: registry, hook: path.join(dir, "hook"), legacy: path.join(dir, "legacy"),
    init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"),
    registrySource: path.join(state, "registry.bin"), gstcool: path.join(dir, "etc/gstcool.conf"),
    core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"),
    command: function (name) { return path.join(bin, name); },
    overrides: function () { return {state: state, gst: gst, owner: path.join(state, "owner"), baseline: path.join(state, "baseline"), recovery: path.join(state, "recovery"), init: path.join(state, "init.sh"), env: path.join(state, "env"), hookSource: path.join(state, "hook.sh"), configSource: path.join(state, "gstcool.conf"), registrySource: path.join(state, "registry.bin"), hook: path.join(dir, "hook"), legacyHook: path.join(dir, "legacy"), payload: payload, payloadts: payloadts, mountinfo: mountinfo, gstTarget: target, gstcool: path.join(dir, "etc/gstcool.conf"), core: path.join(target, "libgstcoreelements.so"), loader: path.join(bin, "loader"), inspect: path.join(bin, "gst-inspect-1.0"), mount: path.join(bin, "mount"), umount: path.join(bin, "umount"), cp: path.join(bin, "cp"), rm: path.join(bin, "rm"), rmdir: path.join(bin, "rmdir"), mkdir: path.join(bin, "mkdir"), mv: path.join(bin, "mv"), chmod: path.join(bin, "chmod"), ln: path.join(bin, "ln"), readlink: "/usr/bin/readlink", sed: path.join(bin, "sed")}; }
  };
}
function run(f, script, extra) { return sh(script, Object.assign({PATH: f.bin + ":" + process.env.PATH, GST_REGISTRY_1_0: f.registry}, extra || {})); }
function ok(result, message) { assert.equal(result.status, 0, message + "\n" + result.stderr + "\n" + result.stdout); }

function initFn(name) {
  var body = service.w25InitScriptBody().split("\n");
  var fn = body.slice(body.indexOf(name + "() {"));
  return fn.slice(0, fn.indexOf("}") + 1).join("\n");
}

module.exports = {ROOT, SERVICE, loadService, service, C, sh, write, exe, fixture, run, ok, initFn};
