# Releasing

## Versioning

The version lives in two places: `CMakeLists.txt` (`project(... VERSION x.y.z)`) and `vcpkg.json` (`version-string`). Bump both together.

## Publishing a release

1. Run `./scripts/release.sh <major|minor|patch|X.Y.Z>` on `main`. It bumps `CMakeLists.txt` and `vcpkg.json` on a `chore/release-X.Y.Z` branch, opens a PR (labeled `ignore-for-release`), waits for its checks, squash-merges it, then tags and pushes `vX.Y.Z`. That push triggers `release.yml` to build the plugin, package it via `scripts/package.sh`, and publish a GitHub Release with the zip and PDB. The `CMakeLists.txt` version is also embedded in the DLL through the `SKSEPlugin_Version` that `add_commonlibsse_plugin` generates, and `release.yml` names the zip after the tag. Keep all three (files, tag, DLL) in lockstep. Requires the GitHub CLI (`gh`), authenticated with push/merge access.
2. `nexus-upload.yml` auto-triggers once `release.yml` finishes. Approve the pending deployment under the `nexus` environment (on the workflow run's page, or the repo's Environments tab) to let the upload proceed. If it doesn't fire, or you need to re-run a failed upload, trigger it manually via workflow_dispatch with the version (no `v` prefix). It must match the tag exactly, since the workflow resolves the tag from the checked-out commit and fails outright if that tag doesn't exist. See [Nexus Mods upload](#nexus-mods-upload) for the environment setup.
3. If the Nexus page changed, regenerate it with `python3 scripts/generate-nexus-page.py` and paste the output into the Nexus Mods page editor.

## Nexus Mods upload

`nexus-upload.yml` runs on a `workflow_run` trigger chained off `release.yml`: `release.yml` publishes the GitHub Release using the default `GITHUB_TOKEN`, and GitHub doesn't run a `release: published`-triggered workflow off events caused by `GITHUB_TOKEN`, so `workflow_run` is used instead (it fires on the completion of `release.yml` itself, regardless of what token published inside it). The `upload` job runs under the `nexus` [GitHub environment](https://docs.github.com/en/actions/deployment/targeting-different-environments/using-environments-for-deployment), which requires manual approval before the job proceeds and scopes the Nexus secrets to that environment. It can also be triggered manually via **workflow_dispatch** with the version (no `v` prefix), e.g. to re-run a failed upload; this still requires the same environment approval.

Because `workflow_run` always runs the copy of `nexus-upload.yml` committed to `main` (not whatever's on a feature branch), changes to this workflow only take effect after merging to `main`.

Do not restrict the `nexus` environment to tags: `workflow_run`-triggered jobs always execute against the default branch's ref (`refs/heads/main`), not the tag that triggered the upstream `release.yml` run, so a tag-only policy would silently block every auto-triggered upload. A deployment-branch policy limited to `main` is compatible with the auto-trigger and keeps `workflow_dispatch` runs from other branches from reaching the secrets.

**Prerequisites (one-time setup):**

1. Upload your first file manually via the [Nexus Mods web UI](https://www.nexusmods.com): this creates the file that later uploads add versions to.
2. Note its file ID from the **API Info** option on the mod page's Files tab, or from the file's edit menu on the Manage Files page.
3. Create the `nexus` environment (Settings → Environments → New environment) with a required reviewer, before the workflow referencing it is merged to `main`. Otherwise GitHub auto-creates it unprotected on first reference.
4. Add to the `nexus` environment as secrets (Settings → Environments → `nexus` → Environment secrets):
   - `NEXUSMODS_API_KEY`: your Nexus Mods API key
   - `NEXUSMODS_FILE_ID`: the file ID
   - `NEXUSMODS_MOD_ID`: the mod's internal ID, used to post each release's notes to the mod's Changelog tab. **Not** the number in the mod page URL. Look that URL number up via `https://api.nexusmods.com/v3/games/skyrimspecialedition/mods/<url-id>` (needs an `apikey` header) and use the `id` field from the response.
5. Add to the `nexus` environment as a variable (Settings → Environments → `nexus` → Environment variables):
   - `NEXUSMODS_DISPLAY_NAME`: the file name shown on Nexus
