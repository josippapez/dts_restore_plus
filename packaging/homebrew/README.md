# Publishing to the webOS Homebrew Channel

The Homebrew Channel (HBC) uses a **two-file model**:

1. A **manifest JSON** hosted on our GitHub release
   (`io.github.josippapez.dtsenabler.manifest.json`) — carries the version, `ipkUrl`
   (bare filename, resolved against the manifest URL), and the ipk `sha256`. This
   is **generated and uploaded automatically by the release workflow**
   (`.github/workflows/release.yml`) on every tag.
2. A **registry YAML** (`io.github.josippapez.dtsenabler.yml`, this folder) submitted to
   the official [`webosbrew/apps-repo`](https://github.com/webosbrew/apps-repo) —
   points HBC at the manifest via
   `…/releases/latest/download/io.github.josippapez.dtsenabler.manifest.json` (so future
   releases update automatically, no new PR needed).

## Option A — official listing (appears in HBC for everyone)

1. Fork `webosbrew/apps-repo`.
2. Copy this file to `packages/io.github.josippapez.dtsenabler.yml` in the fork.
3. Open a PR. Their CI (`repogen.lintpkg` + `downloadipk` sha256 check +
   `check_compat`) must pass — so the release manifest/ipk URLs must be public and
   the hash correct (they are, produced by our workflow).
4. On merge, "DTS Enabler" shows up in the Homebrew Channel app list.

> `category` may need to match apps-repo's allowed set — adjust if lint flags it.

## Option B — self-hosted repo (LIVE — install now, no PR)

A ready-to-use repository is published via GitHub Pages from this fork's
`gh-pages` branch:

> **Repository URL:** `https://josippapez.github.io/dts_restore_plus/api/apps.json`

In the Homebrew Channel: **Settings → Add repository →** paste that URL. "DTS
Enabler" then appears in the list and installs (the `.ipk` is pulled from the
GitHub release and sha256-verified).

### Regenerating the repo after a new release

The static API tree under `gh-pages/api/` embeds the manifest. After cutting a new
release, regenerate and push it:

```sh
git clone https://github.com/webosbrew/apps-repo && cd apps-repo
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
mkdir mypackages && cp <this-repo>/packaging/homebrew/io.github.josippapez.dtsenabler.yml mypackages/
# generate ONLY the API (content/schemas ships with the clone; don't delete it):
python3 -c "from pathlib import Path; from repogen import pkg_info, apidata; \
  apidata.generate(pkg_info.list_packages(Path('mypackages')), Path('content/api'))"
# publish content/api as gh-pages/api on dts_restore_plus (keep .nojekyll)
```

(Because the registry `manifestUrl` uses `releases/latest/download/…`, HBC always
fetches the newest manifest; regeneration only refreshes the embedded copy in the
listing.)

## Updating

Cut a new release (`git tag webos25-<X.Y> && git push …`). The workflow rebuilds
the `.ipk`, regenerates the manifest with the new version + hash, and uploads both
to the release. Because the registry's `manifestUrl` points at
`releases/latest/download/…`, HBC picks up the new version automatically — no
apps-repo change required.
