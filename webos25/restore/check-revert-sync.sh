#!/bin/sh
# check-revert-sync.sh -- sibling to check-init-sync.sh. The W25-COMPAT block
# (the gate) is authored once and spliced everywhere, but install.sh and
# uninstall.sh still hand-duplicate two OTHER pieces of the same revert logic
# that check-init-sync.sh does not cover:
#   1. the SET of bind targets each unmounts/reverts (w25_umount over a
#      "for VAR in "$A" "$B" ...; do" loop) -- init_dts25.sh, install.sh and
#      uninstall.sh are ALL known to duplicate this today, so for these three
#      files failing to extract it is treated as a setup problem (exit 2: the
#      extractor below almost certainly needs updating after a reformat), not
#      as "the duplication went away" -- silently skipping it would let this
#      guard stop guarding without ever failing.
#   2. the GST_PLUGIN_PATH_1_0 used to regenerate a clean STOCK registry
#      (distinct from the "OUR" regen, which additionally lists our own
#      plugin dirs -- identifiable by NOT mentioning "webosbrew"). Unlike (1),
#      install.sh genuinely does not duplicate this piece at all -- it just
#      runs the canonical script, which does its own regen -- so an absent
#      extraction there is a real, permanent skip, not a setup problem.
#
# Missing one bind target in uninstall.sh is exactly "not reversible"; a
# divergent stock GST_PLUGIN_PATH_1_0 would regenerate a wrong registry. This
# compares the EXTRACTED SETS/VALUES, not whole lines, so harmless formatting
# differences (quoting, line-wrapping, variable name) never produce a false
# failure -- only a real difference in the resolved paths does.
#
# Usage: sh webos25/restore/check-revert-sync.sh
#   (also invoked automatically by check-init-sync.sh; run from anywhere --
#   paths are resolved relative to this script's own location)
#
# Exit codes: 0 = in sync. 1 = a genuine invariant violation (a resolved path
# actually differs). 2 = a setup problem -- either a missing file, or one of
# init_dts25.sh/install.sh/uninstall.sh no longer matching the bind-target
# loop shape this script looks for (almost certainly a reformat the extractor
# needs updating for, not evidence the duplication was removed).
#
# Requires: POSIX sh, node. Runs on macOS and Linux.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

CANONICAL="$REPO_ROOT/webos25/restore/init_dts25.sh"
INSTALL_SH="$REPO_ROOT/webos25/restore/install.sh"
UNINSTALL_SH="$REPO_ROOT/webos25/restore/uninstall.sh"

for f in "$CANONICAL" "$INSTALL_SH" "$UNINSTALL_SH"; do
  if [ ! -f "$f" ]; then
    echo "check-revert-sync: missing expected file: $f" >&2
    exit 2
  fi
done

node -e '
  var fs = require("fs");
  var canonicalPath = process.argv[1];
  var installPath = process.argv[2];
  var uninstallPath = process.argv[3];

  var labels = {
    "init_dts25.sh": canonicalPath,
    "install.sh": installPath,
    "uninstall.sh": uninstallPath
  };
  var files = {
    "init_dts25.sh": fs.readFileSync(canonicalPath, "utf8"),
    "install.sh": fs.readFileSync(installPath, "utf8"),
    "uninstall.sh": fs.readFileSync(uninstallPath, "utf8")
  };

  function uniqueSorted(arr) {
    var seen = {};
    var out = [];
    arr.forEach(function (v) {
      if (!Object.prototype.hasOwnProperty.call(seen, v)) { seen[v] = true; out.push(v); }
    });
    out.sort();
    return out;
  }

  function resolveVar(text, name) {
    var re = new RegExp("^" + name + "=(.*)$", "m");
    var m = re.exec(text);
    return m ? m[1] : null;
  }

  // setupFailures: the extractor could not make sense of a file it is known to
  // apply to -- almost certainly needs updating after a reformat. Reported as
  // exit 2, distinct from a genuine invariant violation (exit 1), because the
  // two want different fixes: one is "update check-revert-sync.sh", the other
  // is "fix the drifted file".
  var setupFailures = [];
  var violations = [];

  // ---- invariant 1: the SET of bind targets each file unmounts/reverts ----
  // init_dts25.sh, install.sh and uninstall.sh are ALL known to duplicate this
  // loop today, so a failed extraction on any of the three is a setup problem,
  // never a silent skip.
  function extractBindTargets(text) {
    var loopRe = /^[ \t]*for \w+ in ((?:"\$[A-Za-z_][A-Za-z0-9_]*"\s*)+); do$/m;
    var m = loopRe.exec(text);
    if (!m) return null; // this file has no recognizable bind-target loop
    var varNames = [];
    var varRe = /"\$([A-Za-z_][A-Za-z0-9_]*)"/g;
    var vm;
    while ((vm = varRe.exec(m[1])) !== null) varNames.push(vm[1]);
    var values = [];
    var unresolved = [];
    varNames.forEach(function (name) {
      var v = resolveVar(text, name);
      if (v === null) unresolved.push(name); else values.push(v);
    });
    return { values: values, unresolved: unresolved };
  }

  var BIND_TARGET_FILES = ["init_dts25.sh", "install.sh", "uninstall.sh"];
  var bindSets = {};
  BIND_TARGET_FILES.forEach(function (label) {
    var extracted = extractBindTargets(files[label]);
    if (extracted === null) {
      setupFailures.push(
        "bind-target invariant: " + labels[label] + " has no recognizable bind-target loop " +
        "(a \"for VAR in \\\"$A\\\" ...; do\" calling w25_umount) any more. " + labels[label] +
        " is known to duplicate this today, so this almost certainly means the loop was " +
        "reformatted (wrapped onto another line, \"do\" moved, etc.) -- update the extractor " +
        "in check-revert-sync.sh to match, rather than assume the duplication went away."
      );
      return;
    }
    if (extracted.unresolved.length > 0) {
      setupFailures.push(
        "bind-target invariant: " + labels[label] + " could not resolve bind-target variable(s) " +
        extracted.unresolved.join(", ") + " -- update the extractor in check-revert-sync.sh to match."
      );
      return;
    }
    bindSets[label] = extracted.values;
  });

  if (setupFailures.length === 0) {
    var canonSet = uniqueSorted(bindSets["init_dts25.sh"]);
    ["install.sh", "uninstall.sh"].forEach(function (label) {
      var fileSet = uniqueSorted(bindSets[label]);
      var missing = canonSet.filter(function (v) { return fileSet.indexOf(v) === -1; });
      var extra = fileSet.filter(function (v) { return canonSet.indexOf(v) === -1; });
      if (missing.length > 0 || extra.length > 0) {
        var parts = [];
        if (missing.length) parts.push("is missing " + JSON.stringify(missing));
        if (extra.length) parts.push("has extra " + JSON.stringify(extra));
        violations.push(
          "bind-target invariant: " + labels[label] + " " + parts.join("; ") +
          " compared to canonical " + labels["init_dts25.sh"]
        );
      }
    });
  }

  // ---- invariant 2: the GST_PLUGIN_PATH_1_0 used for a STOCK registry regen,
  // identified by NOT mentioning "webosbrew" (that marks the OTHER regen,
  // which additionally registers our own plugin dirs and is not duplicated).
  // install.sh genuinely has none of its own (it just runs the canonical
  // script) -- that is a real skip, not a setup problem, so it is left as-is.
  function extractStockPluginPaths(text) {
    var re = /GST_PLUGIN_PATH_1_0=(\S+)/g;
    var vals = [];
    var m;
    while ((m = re.exec(text)) !== null) {
      if (m[1].indexOf("webosbrew") === -1) vals.push(m[1]);
    }
    return uniqueSorted(vals);
  }

  var stockVals = {};
  Object.keys(files).forEach(function (label) {
    var vals = extractStockPluginPaths(files[label]);
    if (vals.length === 0) return; // this file does not duplicate a stock regen -- fine, skip it
    if (vals.length > 1) {
      violations.push(
        "stock GST_PLUGIN_PATH_1_0 invariant: " + labels[label] +
        " itself has " + vals.length + " differing stock-flavored values: " + JSON.stringify(vals)
      );
      return;
    }
    stockVals[label] = vals[0];
  });

  var presentLabels = Object.keys(stockVals);
  if (presentLabels.length >= 2) {
    var refLabel = presentLabels[0];
    var refVal = stockVals[refLabel];
    presentLabels.slice(1).forEach(function (label) {
      if (stockVals[label] !== refVal) {
        violations.push(
          "stock GST_PLUGIN_PATH_1_0 invariant: " + labels[label] + " uses \"" + stockVals[label] +
          "\" but " + labels[refLabel] + " uses \"" + refVal + "\""
        );
      }
    });
  }

  if (setupFailures.length > 0) {
    setupFailures.forEach(function (f) { console.log("REVERT-SYNC-SETUP-ERROR: " + f); });
    violations.forEach(function (f) { console.log("REVERT-DRIFT: " + f); });
    process.exit(2);
  }
  if (violations.length > 0) {
    violations.forEach(function (f) { console.log("REVERT-DRIFT: " + f); });
    process.exit(1);
  }
  console.log(
    "check-revert-sync: OK -- bind-target set and stock GST_PLUGIN_PATH_1_0 match across " +
    "init_dts25.sh, install.sh and uninstall.sh (where each duplicates them)."
  );
' "$CANONICAL" "$INSTALL_SH" "$UNINSTALL_SH"
