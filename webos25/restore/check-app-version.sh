#!/bin/sh
# check-app-version.sh -- releases follow semantic versioning, MAJOR.MINOR.PATCH,
# and the tag and the app carry the same number: webos25-2.82.0 ships app
# 2.82.0. A bug fix bumps PATCH, a feature MINOR, a breaking change MAJOR.
# webOS wants exactly three numbers without leading zeroes.
#
# Usage: sh webos25/restore/check-app-version.sh webos25-2.82.0
# Exit 0 = both version files carry the tag's number, 1 = they do not.
set -u
tag=${1:?usage: check-app-version.sh webos25-MAJOR.MINOR.PATCH}
root=$(cd "$(dirname "$0")/../.." && pwd)
want=$(printf '%s\n' "$tag" | sed -nE 's/^(webos25-|v)((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))$/\2/p')
if [ -z "$want" ]; then
  echo "check-app-version: '$tag' is not webos25-MAJOR.MINOR.PATCH (e.g. webos25-2.82.0)" >&2
  exit 1
fi
status=0
for f in webos25/app/appinfo.json webos25/app/service/package.json; do
  have=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$root/$f" | head -n1)
  if [ "$have" != "$want" ]; then
    echo "check-app-version: $f has version '$have', $tag needs '$want'" >&2
    status=1
  fi
done
[ "$status" -eq 0 ] && echo "app version $want matches $tag"
exit "$status"
