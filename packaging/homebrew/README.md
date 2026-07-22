# Publishing to the webOS Homebrew Channel

The Homebrew Channel (HBC) uses a **two-file model**:

1. A **manifest JSON** hosted on our GitHub release
   (`org.webosbrew.dtsenabler.manifest.json`) — carries the version, `ipkUrl`
   (bare filename, resolved against the manifest URL), and the ipk `sha256`. This
   is **generated and uploaded automatically by the release workflow**
   (`.github/workflows/release.yml`) on every tag.
2. A **registry YAML** (`org.webosbrew.dtsenabler.yml`, this folder) submitted to
   the official [`webosbrew/apps-repo`](https://github.com/webosbrew/apps-repo) —
   points HBC at the manifest via
   `…/releases/latest/download/org.webosbrew.dtsenabler.manifest.json` (so future
   releases update automatically, no new PR needed).

## Option A — official listing (appears in HBC for everyone)

1. Fork `webosbrew/apps-repo`.
2. Copy this file to `packages/org.webosbrew.dtsenabler.yml` in the fork.
3. Open a PR. Their CI (`repogen.lintpkg` + `downloadipk` sha256 check +
   `check_compat`) must pass — so the release manifest/ipk URLs must be public and
   the hash correct (they are, produced by our workflow).
4. On merge, "DTS Enabler" shows up in the Homebrew Channel app list.

> `category` may need to match apps-repo's allowed set — adjust if lint flags it.

## Option B — self-hosted repo (install now, no PR)

Run webosbrew's `repogen` over a `packages/` dir containing this YAML to produce a
static API tree, host it, and add its base URL in HBC → Settings → **Add
repository**. Or deep-link HBC with
`{"launchMode":"addRepository","url":"https://<your-repo>/"}`.

## Updating

Cut a new release (`git tag webos25-<X.Y> && git push …`). The workflow rebuilds
the `.ipk`, regenerates the manifest with the new version + hash, and uploads both
to the release. Because the registry's `manifestUrl` points at
`releases/latest/download/…`, HBC picks up the new version automatically — no
apps-repo change required.
