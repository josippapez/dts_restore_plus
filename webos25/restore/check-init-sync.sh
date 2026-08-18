#!/bin/sh
# check-init-sync.sh -- verify the three hand-synced copies of the webOS25 boot
# script are byte-identical. See RELEASING.md.
#
# The boot script (runs at boot via /var/lib/webosbrew/init.d/restore_dts25)
# exists in three places that must be kept in lockstep by hand:
#   1. webos25/restore/init_dts25.sh                     -- canonical
#   2. the base64 INIT_B64 heredoc payload in webos25/restore/install.sh
#   3. the string built by w25InitScriptBody() in webos25/app/service/service.js
#
# If they drift, the CLI installer, the TV's already-installed hook, and the
# Homebrew app would each be running a DIFFERENT boot script. This guard
# decodes/evaluates copies 2 and 3 and diffs each against copy 1 (the
# canonical file), printing a unified diff and exiting non-zero the moment
# any copy disagrees -- naming which one drifted.
#
# Usage: sh webos25/restore/check-init-sync.sh
#   (run from anywhere -- paths are resolved relative to this script's own
#   location, not the caller's cwd)
#
# Requires: POSIX sh, node, base64, diff. Runs on macOS and Linux (both are
# present in the release workflow image).

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

CANONICAL="$REPO_ROOT/webos25/restore/init_dts25.sh"
INSTALL_SH="$REPO_ROOT/webos25/restore/install.sh"
SERVICE_JS="$REPO_ROOT/webos25/app/service/service.js"

for f in "$CANONICAL" "$INSTALL_SH" "$SERVICE_JS"; do
  if [ ! -f "$f" ]; then
    echo "check-init-sync: missing expected file: $f" >&2
    exit 2
  fi
done

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT INT TERM

FROM_INSTALL="$WORKDIR/from_install.sh"
FROM_SERVICE="$WORKDIR/from_service.sh"

# --- copy 2: decode the INIT_B64 heredoc payload in install.sh -------------
marker_line=$(grep -n 'base64 -d > "\$INIT_SCRIPT" <<' "$INSTALL_SH" | head -n1 | cut -d: -f1)
if [ -z "$marker_line" ]; then
  echo "check-init-sync: could not find the INIT_B64 heredoc marker in $INSTALL_SH" >&2
  exit 2
fi
blob_line=$((marker_line + 1))
if ! sed -n "${blob_line}p" "$INSTALL_SH" | base64 -d > "$FROM_INSTALL" 2>/dev/null; then
  echo "check-init-sync: failed to base64-decode $INSTALL_SH line $blob_line" >&2
  exit 2
fi

# --- copy 3: evaluate w25InitScriptBody() in isolation ---------------------
# service.js requires("webos-service"), which is not installed in CI, so we
# never load the module -- read the file as text, slice out just the
# function's source, and evaluate that in isolation. The function composes
# its result from module-level `var NAME = [...]` array literals (as of this
# writing: W25_INIT_HEAD, W25_COMPAT_SH, W25_INIT_MAIN) -- discover which such
# identifiers the sliced function actually references (generically, not
# hardcoded to those three names) and prepend just those declarations before
# evaluating it, so a future rename/reshape is picked up automatically. Any
# referenced identifier that cannot be resolved this way fails loudly with its
# name instead of evaluating into a bare ReferenceError.
if ! node -e '
  var fs = require("fs");
  var path = process.argv[1];
  var out = process.argv[2];
  var src = fs.readFileSync(path, "utf8");
  var marker = "function w25InitScriptBody()";
  var start = src.indexOf(marker);
  if (start === -1) {
    console.error("w25InitScriptBody() not found in " + path);
    process.exit(2);
  }
  var i = src.indexOf("{", start);
  if (i === -1) {
    console.error("no function body found for w25InitScriptBody()");
    process.exit(2);
  }
  var depth = 0, end = -1;
  for (; i < src.length; i++) {
    var c = src[i];
    if (c === "{") depth++;
    else if (c === "}") { depth--; if (depth === 0) { end = i; break; } }
  }
  if (end === -1) {
    console.error("unbalanced braces while isolating w25InitScriptBody()");
    process.exit(2);
  }
  var fnSrc = src.slice(start, end + 1);

  var JS_BUILTINS = {
    NaN: 1, Infinity: 1, JSON: 1, Math: 1, RegExp: 1, Boolean: 1, Number: 1,
    String: 1, Array: 1, Object: 1, Error: 1, TypeError: 1, RangeError: 1,
    SyntaxError: 1, Buffer: 1, Promise: 1, Symbol: 1, Map: 1, Set: 1, Date: 1,
    Function: 1, Proxy: 1, Reflect: 1
  };
  var identRe = /(^|[^.\w$])([A-Z][A-Z0-9_]{2,})\b/g;
  var referenced = {};
  var im;
  while ((im = identRe.exec(fnSrc)) !== null) {
    if (!JS_BUILTINS[im[2]]) referenced[im[2]] = true;
  }

  // Per-identifier, anchored search: for each name the function actually
  // references, look specifically for ITS OWN "var NAME = [...];" -- rather
  // than one continuous scan across the whole file -- so an unrelated array
  // elsewhere that terminates differently (e.g. piped through .join(...)
  // instead of ending bare) can never swallow past the identifier we want.
  var missing = [];
  var prelude = "";
  Object.keys(referenced).sort().forEach(function (name) {
    var re = new RegExp("var " + name + " = (\\[[\\s\\S]*?\\n\\]);\\n");
    var m = re.exec(src);
    if (m) {
      prelude += "var " + name + " = " + m[1] + ";\n";
    } else {
      missing.push(name);
    }
  });

  if (missing.length > 0) {
    console.error(
      "check-init-sync: w25InitScriptBody() references " + missing.join(", ") +
      " but no top-level \"var NAME = [ ...array... ];\" declaration for " +
      (missing.length === 1 ? "it" : "them") + " was found in " + path +
      ". service.js was refactored -- update check-init-sync.sh to match."
    );
    process.exit(2);
  }

  var fn = new Function(prelude + "return (" + fnSrc + ")")();
  fs.writeFileSync(out, fn());
' "$SERVICE_JS" "$FROM_SERVICE"; then
  echo "check-init-sync: failed to isolate/evaluate w25InitScriptBody() from $SERVICE_JS" >&2
  exit 2
fi

# --- compare against the canonical copy ------------------------------------
# Diagnostics (diff + DRIFT labels) go to stdout; only setup/usage errors
# above go to stderr, so a passing/failing run's findings stay in one stream.
status=0

if ! diff -u "$CANONICAL" "$FROM_INSTALL"; then
  echo ""
  echo "DRIFT: webos25/restore/install.sh (INIT_B64 payload) no longer matches init_dts25.sh"
  status=1
fi

if ! diff -u "$CANONICAL" "$FROM_SERVICE"; then
  echo ""
  echo "DRIFT: webos25/app/service/service.js (w25InitScriptBody) no longer matches init_dts25.sh"
  status=1
fi

# --- also assert the duplicated revert logic (bind-target set + the stock
# registry regen's GST_PLUGIN_PATH_1_0) stays in sync across install.sh /
# uninstall.sh / the canonical script -- a different drift than the boot
# script's three copies above, but just as silent if unchecked. ------------
if ! sh "$SCRIPT_DIR/check-revert-sync.sh"; then
  status=1
fi

if [ "$status" -eq 0 ]; then
  echo "check-init-sync: OK -- all three copies of the boot script match, and the duplicated revert logic (bind targets, stock registry regen) stays in sync."
fi

exit "$status"
