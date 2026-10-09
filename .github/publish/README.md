# Publishing releases to unlayered3d.com

`publish-software.mjs` is a copy of `scripts/publish-software.mjs` from the site's repository, which
is private. That copy is the source of truth: when it changes, copy it here again. It needs only
`@vercel/blob`, pinned in this folder's `package.json`.

A tagged release (`release.yml`, tag `v0.1.2` or `v0.1.2-beta1`) publishes its Windows installer as
the site's **stable** build. That is the download on unlayered3d.com/software, and the update the
in-app update check offers every installed copy (the site builds `/slicer/latest.json` from it). A
`-beta1` style tag is published as a prerelease. `release-notes.md`, if present, becomes the update
dialog's notes.

Dev builds don't come from here: they're built and published by the private development repository.

## Settings

| Name | Kind | What |
|---|---|---|
| `UNLAYERED_SOFTWARE_PUBLISH_TOKEN` | secret | The site's stable-channel publish token. Its SHA-256 is `SOFTWARE_PUBLISH_TOKEN_SHA256` in the site's environment. The token alone decides the channel. |
| `UNLAYERED_SITE` | variable | Optional. Defaults to `https://unlayered3d.com`; set it only to test against another server. |
