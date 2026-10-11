# Publishing builds to unlayered3d.com

`publish-software.mjs` is a copy of `scripts/publish-software.mjs` from the site's repository, which
is private. That copy is the source of truth: when it changes, copy it here again. It needs only
`@vercel/blob`, pinned in this folder's `package.json`.

## One `.github` for both repositories

The private Unlayered3D/Unlayered-Slicer-Dev repository is where CI is edited. The public
Unlayered3D/Unlayered-Slicer gets the same `.github` folder, unchanged, in each source snapshot, so
never edit CI in the public repository directly. Every workflow works in both. Where one should only
run in one of them, it checks the repository by name:

| | Dev (private) | Public |
|---|---|---|
| Build all, tests, checks | yes | yes |
| Dev builds to /software/beta (`nightly_dev.yml`, `publish_dev.yml`, Build all's `publish`) | yes | skipped |
| Stable releases (`release.yml` on a tag) | dry runs only | yes |

Releases build in the public repository because Actions minutes are free there. A tag pushed in
Dev builds nothing.

## Dev builds (Dev repository)

`/software/beta`, site admins only for now.

- `nightly_dev.yml` runs every night. If `main` has a commit that isn't published yet, it starts
  **Build all** on `main` with `publish` on.
- Run **Build all** by hand (Actions → Build all → Run workflow) on any branch with `publish`
  ticked to publish that branch's build. Each build is labelled with its branch on the site.
- `publish_dev.yml` does the publishing. Run it by hand with an earlier Build all run's ID to publish
  that run's installers without rebuilding.

Dev versions are `<UNLAYERED_VERSION from version.inc>-dev+<short commit>`. Installers published:
Windows x64 and ARM64, macOS Universal, Linux x64 and ARM64 AppImages. The site keeps the newest 30
dev versions; delete one on /admin/software.

## Stable releases (public repository)

A tagged release (`release.yml`, tag `v0.1.2` or `v0.1.2-beta1`) publishes its Windows installer as
the site's **stable** build. That is the download on unlayered3d.com/software, and the update the
in-app update check offers every installed copy (the site builds `/slicer/latest.json` from it). A
`-beta1` style tag is published as a prerelease. `release-notes.md`, if present, becomes the update
dialog's notes.

Downloads come from the site only. The GitHub Release is made after the site publish, carries the
tag and changelog with no files attached, and links to unlayered3d.com/software; the run's artifacts
are then deleted, apart from the debug symbols. A dry run (Run workflow with `dry_run`, in either
repository) keeps its artifacts for inspection and publishes nothing.

## Settings

Set in each repository separately. The secret has the same name in both, and a different value:
the token alone decides the channel a build goes to.

| Name | Kind | Dev | Public |
|---|---|---|---|
| `UNLAYERED_SOFTWARE_PUBLISH_TOKEN` | secret | The site's dev-channel token (its SHA-256 is `SOFTWARE_DEV_PUBLISH_TOKEN_SHA256` in Vercel) | The site's stable-channel token (`SOFTWARE_PUBLISH_TOKEN_SHA256`) |
| `UNLAYERED_SITE` | variable | Optional. Defaults to `https://unlayered3d.com`; set it only to test against another server. | Same |
| `DELETE_ARTIFACTS_AFTER_PUBLISH` | variable | Optional. `true` deletes a run's downloadable builds from GitHub once they are all published, so they can only be downloaded from the site. Flatpaks are kept: the site doesn't host them. | Not used: releases always delete theirs. |
