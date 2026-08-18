#!/bin/sh
# sync-init.sh -- the inverse of check-init-sync.sh. Regenerates copies 2 and 3
# of the boot script FROM the canonical copy, so a human only ever edits one
# file:
#   1. webos25/restore/init_dts25.sh                     -- canonical (edit this)
#   2. the base64 INIT_B64 heredoc payload in webos25/restore/install.sh
#   3. W25_INIT_HEAD / W25_COMPAT_SH / W25_INIT_MAIN in webos25/app/service/service.js
#
# Usage: sh webos25/restore/sync-init.sh
#   (run from anywhere -- paths are resolved relative to this script's own
#   location, not the caller's cwd, same as check-init-sync.sh)
#
# Typical flow to add a verified TV, or make any other boot-script change:
#   1. Edit webos25/restore/init_dts25.sh only.
#   2. sh webos25/restore/sync-init.sh
#   3. sh webos25/restore/check-init-sync.sh   # confirms all three now match
#
# Idempotent: running it on an already-in-sync tree writes nothing (the
# generated content is compared to what is on disk first; a file is only
# rewritten if it actually changed). Refuses -- exits non-zero, writes nothing
# to either file -- if it cannot locate every target precisely (the INIT_B64
# heredoc marker in install.sh; the W25-COMPAT-BEGIN/END markers and each of
# the three "var NAME = [...];" declarations in service.js), rather than ever
# half-writing a file.
#
# Also refuses (same "write nothing" guarantee) if the canonical script's body
# changed since the last sync but its hand-maintained W25_GATE_VERSION stamp
# did not: that stamp is what lets the app notice a boot script it installed is
# stale, so a body change with no version bump would make that detection go
# quiet. This is a convenience check, not a safety net -- the app separately
# md5-cross-checks what it would write -- so it is a simple before/after
# string compare against install.sh's CURRENT (pre-regen) blob, done once per
# run; no cache or persisted state.
#
# Requires: POSIX sh, node. Runs on macOS and Linux (both are present in the
# release workflow image).

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

CANONICAL="$REPO_ROOT/webos25/restore/init_dts25.sh"
INSTALL_SH="$REPO_ROOT/webos25/restore/install.sh"
SERVICE_JS="$REPO_ROOT/webos25/app/service/service.js"

for f in "$CANONICAL" "$INSTALL_SH" "$SERVICE_JS"; do
  if [ ! -f "$f" ]; then
    echo "sync-init: missing expected file: $f" >&2
    exit 2
  fi
done

node -e '
  var fs = require("fs");
  var canonicalPath = process.argv[1];
  var installPath = process.argv[2];
  var servicePath = process.argv[3];

  function fail(msg) {
    console.error("sync-init: " + msg);
    process.exit(2);
  }

  var canonical = fs.readFileSync(canonicalPath, "utf8");
  var canonicalLines = canonical.split("\n");
  // A trailing "\n" in the file leaves one trailing "" element after split();
  // drop it so canonicalLines holds exactly one entry per real line.
  if (canonicalLines.length > 0 && canonicalLines[canonicalLines.length - 1] === "") {
    canonicalLines.pop();
  }

  var BEGIN = "# >>> W25-COMPAT-BEGIN";
  var END = "# <<< W25-COMPAT-END";
  var beginIdxs = [];
  var endIdxs = [];
  canonicalLines.forEach(function (l, idx) {
    if (l === BEGIN) beginIdxs.push(idx);
    if (l === END) endIdxs.push(idx);
  });
  if (beginIdxs.length !== 1) {
    fail("expected exactly one \"" + BEGIN + "\" line in " + canonicalPath + ", found " + beginIdxs.length);
  }
  if (endIdxs.length !== 1) {
    fail("expected exactly one \"" + END + "\" line in " + canonicalPath + ", found " + endIdxs.length);
  }
  var beginIdx = beginIdxs[0];
  var endIdx = endIdxs[0];
  if (!(beginIdx < endIdx)) {
    fail("\"" + BEGIN + "\" must come before \"" + END + "\" in " + canonicalPath);
  }

  var headLines = canonicalLines.slice(0, beginIdx);
  var compatLines = canonicalLines.slice(beginIdx, endIdx + 1);
  var mainLines = canonicalLines.slice(endIdx + 1);

  function arrayLiteral(name, lines) {
    var body = lines.map(function (l) { return "  " + JSON.stringify(l); }).join(",\n");
    return "var " + name + " = [\n" + body + "\n];\n";
  }

  var newBlocks = {
    W25_INIT_HEAD: arrayLiteral("W25_INIT_HEAD", headLines),
    W25_COMPAT_SH: arrayLiteral("W25_COMPAT_SH", compatLines),
    W25_INIT_MAIN: arrayLiteral("W25_INIT_MAIN", mainLines)
  };

  // ---- install.sh: locate + replace the INIT_B64 blob line ----------------
  var installText = fs.readFileSync(installPath, "utf8");
  var installLines = installText.split("\n");
  var markerNeedle = "base64 -d > \"$INIT_SCRIPT\" <<";
  var markerIdxs = [];
  installLines.forEach(function (l, idx) {
    if (l.indexOf(markerNeedle) !== -1) markerIdxs.push(idx);
  });
  if (markerIdxs.length !== 1) {
    fail("expected exactly one INIT_B64 heredoc marker line in " + installPath + ", found " + markerIdxs.length);
  }
  var blobIdx = markerIdxs[0] + 1;
  if (blobIdx >= installLines.length) {
    fail("no blob line found after the INIT_B64 heredoc marker in " + installPath);
  }
  var terminatorIdx = blobIdx + 1;
  if (installLines[terminatorIdx] !== "INIT_B64") {
    fail(
      "expected the line after the INIT_B64 blob in " + installPath +
      " to be the literal heredoc terminator \"INIT_B64\", found: " +
      JSON.stringify(installLines[terminatorIdx])
    );
  }

  // ---- W25_GATE_VERSION staleness-stamp check ------------------------------
  // Compare the canonical body (minus the stamp line) against what install.sh
  // CURRENTLY decodes to (i.e. the body before this run regenerates anything):
  // if the body actually changed but the stamp is identical on both sides,
  // someone edited the boot script and forgot to bump it. A single in-memory
  // string compare -- no cache, no persisted state.
  function extractGateVersion(text) {
    var m = /^W25_GATE_VERSION=(.*)$/m.exec(text);
    return m ? m[1] : null;
  }
  function stripGateVersionLine(text) {
    return text.replace(/^W25_GATE_VERSION=.*\n/m, "");
  }
  var oldBody = Buffer.from(installLines[blobIdx], "base64").toString("utf8");
  var canonicalGateVersion = extractGateVersion(canonical);
  var oldGateVersion = extractGateVersion(oldBody);
  if (canonicalGateVersion !== null && oldGateVersion !== null && canonicalGateVersion === oldGateVersion) {
    if (stripGateVersionLine(canonical) !== stripGateVersionLine(oldBody)) {
      fail(
        "the canonical script body changed but W25_GATE_VERSION is still " + canonicalGateVersion +
        " in " + canonicalPath + " -- bump it before running sync-init.sh again " +
        "(a stale stamp is how the app would stop noticing an out-of-date install)."
      );
    }
  }
  // If either side has no W25_GATE_VERSION line at all, this check does not
  // apply (nothing to compare) -- proceed; it is a convenience, not a gate.

  var newB64 = Buffer.from(canonical, "utf8").toString("base64");
  var newInstallLines = installLines.slice();
  newInstallLines[blobIdx] = newB64;
  var newInstallText = newInstallLines.join("\n");

  // ---- service.js: locate + replace each of the three array literals ------
  var serviceText = fs.readFileSync(servicePath, "utf8");
  var newServiceText = serviceText;
  ["W25_INIT_HEAD", "W25_COMPAT_SH", "W25_INIT_MAIN"].forEach(function (name) {
    var re = new RegExp("var " + name + " = \\[[\\s\\S]*?\\n\\];\\n", "g");
    var matches = newServiceText.match(re);
    if (!matches || matches.length !== 1) {
      fail(
        "expected exactly one \"var " + name + " = [ ...array... ];\" declaration in " +
        servicePath + ", found " + (matches ? matches.length : 0)
      );
    }
    newServiceText = newServiceText.replace(re, function () { return newBlocks[name]; });
  });

  // ---- write only what actually changed, and only after every target above
  // resolved precisely (nothing is written if any fail() above already exited) ----
  var writes = [];
  if (newInstallText !== installText) {
    writes.push({ path: installPath, text: newInstallText, label: "install.sh (INIT_B64 blob)" });
  }
  if (newServiceText !== serviceText) {
    writes.push({
      path: servicePath, text: newServiceText,
      label: "service.js (W25_INIT_HEAD / W25_COMPAT_SH / W25_INIT_MAIN)"
    });
  }

  writes.forEach(function (w) { fs.writeFileSync(w.path, w.text); });

  if (writes.length === 0) {
    console.log("sync-init: already in sync -- nothing to do.");
  } else {
    writes.forEach(function (w) { console.log("sync-init: updated " + w.label); });
  }
' "$CANONICAL" "$INSTALL_SH" "$SERVICE_JS"
