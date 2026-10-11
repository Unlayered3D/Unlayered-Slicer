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
| Dev builds (`nightly_dev.yml`, `publish_dev.yml`, Build all's `publish`) | yes | skipped |
| Release (`release.yml`): dry-run and staging | yes | yes |
| Release (`release.yml`): release | no | yes |

Releases build in the public repository because Actions minutes are free there. A tag pushed in
Dev builds nothing.

## Where builds go on the site

The token a run holds decides the channel; nothing in a request can pick another one.

| Channel | Who sees it | Fed by |
|---|---|---|
| stable | /software for every account, and the in-app update check | `release.yml`, mode `release` |
| staging | Site admins on /admin/software | `release.yml`, mode `staging` |
| dev | Site admins on /admin/software | Dev's Build all with `publish` |

Admins can share any build with one person: **Share** on /admin/software makes a link that works only
for the email it was made for, for 1, 7 or 30 days.

## Dev builds (Dev repository)

- `nightly_dev.yml` runs every night. If `main` has a commit that isn't published yet, it starts
  **Build all** on `main` with `publish` on.
- Run **Build all** by hand (Actions → Build all → Run workflow) on any branch with `publish`
  ticked to publish that branch's build. Each build is labelled with its branch on the site.
- `publish_dev.yml` does the publishing. Run it by hand with an earlier Build all run's ID to publish
  that run's installers without rebuilding.

Dev versions are `<UNLAYERED_VERSION from version.inc>-dev+<short commit>`. Installers published:
Windows x64 and ARM64, macOS Universal, Linux x64 and ARM64 AppImages. The site keeps the newest 30
dev versions, plus any with a live share link; delete one on /admin/software.

## Releases (`release.yml`)

A tag push (`v0.1.2`, or `v0.1.2-beta1` for a prerelease) is always a release. Run by hand
(Actions → Release → Run workflow), it asks for a mode:

- **dry-run**: build the Windows installer and keep it as an artifact. Publishes nothing.
- **staging**: the whole release flow, where nobody downloads it. The installer goes to the site's
  staging channel (admins only; never /software, past builds or the update check), the GitHub
  Release is a **draft** named `STAGING …` under its own `staging-…` tag (only maintainers see
  drafts), and the run's artifacts are deleted as a release's would be. Works on any branch or tag.
  Delete the draft afterwards; each staging run replaces the last one on the site, and
  /admin/software can clear it.
- **release**: the real thing, from a tag (pick it under "Use workflow from"). The installer becomes
  the **stable** build: the download on unlayered3d.com/software, and the update the in-app update
  check offers every installed copy (the site builds `/slicer/latest.json` from it).

Downloads come from the site only. The GitHub Release is made after the site publish, carries the
tag and changelog with no files attached, and links to unlayered3d.com/software; the run's artifacts
are then deleted, apart from the debug symbols. `release-notes.md`, if present, becomes the update
dialog's notes.

## Settings

Set in each repository separately. `UNLAYERED_SOFTWARE_PUBLISH_TOKEN` has the same name in both and a
different value in each.

| Name | Kind | Dev | Public |
|---|---|---|---|
| `UNLAYERED_SOFTWARE_PUBLISH_TOKEN` | secret | The site's dev-channel token | The site's stable-channel token |
| `UNLAYERED_SOFTWARE_STAGING_TOKEN` | secret | Optional: the staging token, to run staging here | The site's staging-channel token |
| `UNLAYERED_SITE` | variable | Optional. Defaults to `https://unlayered3d.com`; set it only to test against another server. | Same |
| `DELETE_ARTIFACTS_AFTER_PUBLISH` | variable | Optional. `true` deletes a run's downloadable builds from GitHub once they are all published, so they can only be downloaded from the site. Flatpaks are kept: the site doesn't host them. | Not used: releases always delete theirs. |

A release run checks that its secret holds the token for the channel it means to publish to, and
fails before uploading anything if not.
