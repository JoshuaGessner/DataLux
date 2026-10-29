# Native milestone releases

> **Status:** Accepted milestone workflow; Play production acceptance pending
> **Owner:** Release engineering
> **Last reviewed:** 2026-10-28
> **Related:** PRODUCT.md, DESKTOP_UPDATES_PLAN.md

The proposed in-app desktop update experience and rollout gates are documented in
[DESKTOP_UPDATES_PLAN.md](DESKTOP_UPDATES_PLAN.md). That feature is planned; current v0.2.0 binaries
do not auto-update. Mobile updates will be managed through their app stores.

The current policy is [PRODUCT.md](PRODUCT.md): all desktop features are free; Android Free has
single-file workflows and security, and the $10 USD one-time US Pro plan adds batch send/receive
and bulk actions. Normal Android builds enforce Free/Pro. Only an explicit `-Pstore=false` produces
an unlocked Android community build. Historical community APKs remain unchanged. Production Play
purchases still require pinned public trust, stable signing, real Google setup/tests and approval;
no public sales have been enabled. See `web/PLAY_LAUNCH.md` in the separate website repository.
Browser-assisted PC account sign-in is implemented; see [desktop/ACCOUNTS.md](desktop/ACCOUNTS.md).
The matching account and email-verification service is deployed. Native v0.3.0 introduced account
integration; v0.4.0 adds receiver-side batch enforcement and default Android Free/Pro policy. Older
binaries do not gain features or paywalls automatically. Desktop automatic updates remain a proposal.

FileFlow's native downloads are built only for milestones that the repository owner chooses. Every
owner-dispatched run builds a reviewed source commit. Only a run with explicit publication approval
creates the tag and GitHub Release after every platform build succeeds. Application binaries are
Release assets only: they are never committed to Git or pushed to a package registry.

All packages produced by the current workflow are **Experimental**. Compilation and smoke tests do
not establish broad hardware or platform compatibility.

## Owner release procedure

1. Choose the milestone version and update both `project(... VERSION ...)` in `CMakeLists.txt` and
   the `fileflowVersionName` default in `app/build.gradle.kts`. They must contain the same bare
   SemVer, such as `0.3.0`. Increment the `fileflowVersionCode` default for each Android release.
2. Merge the version change and all intended release work to `main`. Wait for normal CI
   and desktop build checks to pass.
3. Resolve and record the reviewed `main` commit. Use this same immutable 40-character SHA for both
   the dry run and publication; do not pass a moving branch name for an actual release:

   ```sh
   git fetch origin main
   SOURCE_SHA="$(git rev-parse 'origin/main^{commit}')"
   ```

4. From **Actions → native milestone release → Run workflow**, enter the version (prefer
   `vMAJOR.MINOR.PATCH`; bare SemVer is also accepted), the recorded source SHA, and keep
   **publish_release** false for the required build-only dry run. A version such as
   `v0.3.0-beta.1` is published as a GitHub prerelease.
5. After every dry-run platform artifact passes review, run the same version and exact source SHA
   again with **publish_release** explicitly true:

   ```sh
   gh workflow run milestone-release.yml --ref main \
     -f version=v0.3.0 -f source_ref="$SOURCE_SHA" -f publish_release=false
   # Wait for and review the complete dry run before continuing.
   gh workflow run milestone-release.yml --ref main \
     -f version=v0.3.0 -f source_ref="$SOURCE_SHA" -f publish_release=true
   ```

6. Wait for **native milestone release** to finish. Until its final manifest appears, the website
   must not offer the release as a download.

The default `publish_release=false` is a build-only dry run. It produces seven-day GitHub Actions
artifacts for review and creates no tag, GitHub Release, or external download. Only
`publish_release=true` starts the write-scoped publication job. That job uses a draft internally
while uploading and verifying assets, then publishes it before the run succeeds. The workflow never
overwrites Release assets, never rewrites a tag, and always rejects an already published version.

`source_ref` may identify a branch, tag, or commit, but the resolved commit must be reachable from
`origin/main`. The workflow also requires the native source versions to match the requested SemVer
(the core `MAJOR.MINOR.PATCH` for a prerelease).

No release secrets are required. Only the final publication job receives `contents: write` through
the job-scoped `GITHUB_TOKEN`; build jobs are read-only. All referenced actions are pinned to full
commit hashes.

## Published assets

One successful publication run attaches these packages to the workflow-created GitHub Release:

| Platform | Package | Architecture source | Signing status |
| --- | --- | --- | --- |
| Linux | Portable `.tar.gz` | Detected from the build runner | Unsigned |
| Windows | Portable `.zip` | Detected from the build runner | Unsigned |
| macOS | Drag-and-drop `.dmg` with FileFlow.app and an Applications shortcut | Read from the built Mach-O file | Ad-hoc app signature only; not Developer ID signed or notarized |
| Android | Sideloadable debug `.apk` | Fixed project ABI, `arm64-v8a` | Ephemeral runner-generated debug certificate; not a persistent release identity |

The workflow reuses the established Linux, Windows, and Android build/test gates. The macOS release
job builds the production app bundle, launch-smoke-tests it, verifies its ad-hoc signature, and
records whether the actual binary is `x86_64`, `arm64`, or `universal2`. The portable core suite
remains an ordinary CI gate. No architecture is inferred from a mutable runner label.

The DMG uses a branded Finder background and fixed icon positions. Its builder verifies the disk
image, mounts it read-only, checks the Applications link, checks the app's signature and architecture,
and compares the complete app tree with the original bundle. Hidden `.fileflow` documents contain
the source revision, license, and README. A report binds these checks to the DMG's SHA-256 and source
commit before the Linux publication job accepts it. Build tools are hash-pinned in
`desktop/macos/packaging/requirements-dmg.txt`; no Finder UI scripting is required.

GitHub Actions artifacts are retained for only seven days. They are the only output of a build-only
dry run and staging material during a publication run. The verified copies attached to the GitHub
Release are the durable downloads.

## Feed contract and atomic visibility

The canonical feed asset is `fileflow-release-manifest.json`. Schema version 1 is:

```json
{
  "schemaVersion": 1,
  "version": "0.3.0",
  "date": "2026-10-25",
  "artifacts": [
    {
      "name": "FileFlow-v0.3.0-linux-x86_64.tar.gz",
      "platform": "linux",
      "arch": "x86_64",
      "url": "https://github.com/OWNER/REPOSITORY/releases/download/v0.3.0/ASSET",
      "sha256": "64-lowercase-hex-digits",
      "signing": {
        "status": "unsigned",
        "details": "No Linux package signature."
      },
      "notes": "Experimental portable tarball; no installer or distribution package.",
      "knownIssues": ["Compatibility is not validated across Linux distributions."]
    }
  ]
}
```

`version` is bare SemVer; its Git tag and GitHub Release are `v${version}`. `date` is the workflow's
UTC release date; a publication run verifies it equals the GitHub Release's `published_at` date and
removes the manifest if it does not. GitHub gives draft assets temporary `untagged-*` URLs, so the
workflow writes the canonical final tag URL and, immediately after publication, requires it to equal
the exact `browser_download_url` reported by the published Release API. A mismatch removes the
manifest and fails closed. `signing.status` is one of `unsigned`, `ad-hoc`, or `signed`.

The separate `SHA256SUMS` asset covers every package and the manifest itself. Before making the
manifest visible, the workflow:

1. validates the source commit, exact four-package set, and archive contents before creating a tag;
2. creates or reuses an empty draft, refusing a published release or any existing assets;
3. uploads the packages and compares their local SHA-256 values with GitHub's server-computed
   digests;
4. downloads every uploaded package again through the authenticated release-assets API and hashes
   the received bytes;
5. creates and uploads `SHA256SUMS`, then verifies its server digest and downloaded bytes;
6. uploads `fileflow-release-manifest.json` last and verifies it the same way; and
7. publishes the draft only when `publish_release=true`, marking hyphenated SemVer as prerelease;
   and
8. verifies every final manifest URL against the published Release API and verifies the tag still
   resolves to the reviewed source commit.

The download feed must expose a package only when both metadata assets exist and all of these values
agree: manifest name and hash, `SHA256SUMS`, GitHub API asset name and digest, and same-release API
download URL. A draft is never visible in the public feed. A failed publication may leave a draft
or a published release without a manifest, which is also intentionally invisible. Release assets
are treated as immutable. Ordinarily, repair means reviewing the failure and starting a fresh
version or deliberately removing an incomplete draft and tag before rerunning.

### Audited v0.2.0 metadata repair

The first v0.2.0 publication exposed GitHub's temporary draft URLs in the manifest. Those URLs became
404s when GitHub published the draft under its final tag. The owner-only, workflow-dispatch-only
`repair-release-metadata.yml` is narrowly locked to the original v0.2.0 release ID, tag, source commit,
publication date, four package names, and reviewed SHA-256 values. It refuses any other release or an
immutable release. Before writing, it downloads and verifies every package and both original metadata
assets, preserves the original and repaired metadata plus API state as a 90-day Actions audit
artifact, and rechecks that nothing changed.

The repair hides the invalid manifest first, replaces only `SHA256SUMS`, then publishes the corrected
manifest last. It never replaces a package or moves the tag. Any post-write verification failure
removes the manifest so the download feed stays hidden. Because the workflow is locked to the exact
known-bad metadata digests, a second run after a successful repair is rejected.

### v0.2.0 DMG packaging update

The owner-only `macos-dmg.yml` workflow packages the already published, verified v0.2.0 ARM64 Mac app
as a DMG. It checks the original release/tag/source and exact package/metadata digests before building.
`publish_dmg=false` produces reviewable Actions artifacts only. With explicit publication enabled,
it adds the new DMG and changes only the Mac manifest record and checksum metadata. The original
ZIP and all other installer assets keep their IDs and bytes; the source tag does not move.

Metadata is published last and verified by both GitHub's digest and downloaded bytes. A failed
publication job can be rerun against its retained, identical build artifact. It recognizes only
the reviewed old metadata or its own exact replacement bytes, refusing unrelated changes. The
website's normal catalog refresh picks up the DMG without requiring another web image release.

To install, open the DMG and drag FileFlow to Applications, then eject the disk image. This packaging
change does not add Developer ID signing or notarization and does not remove Gatekeeper checks.

## Current limitations and signing gates

- Windows users may see SmartScreen warnings. Do not label Windows Stable until an owner-approved
  Authenticode identity signs the final executable and the signature is verified in the workflow.
- macOS Gatekeeper may block the app. Do not label macOS Stable until a Developer ID identity signs
  the complete app, Apple notarization succeeds, the ticket is stapled, and all three are verified.
- The Android APK is a debug build with a per-run key, not a supported in-place upgrade path.
  Installing another build may require uninstalling the existing app, which removes app-local
  state. Stable Android signing is not configured. Public Android releases require an
  owner-controlled persistent release identity or Play App Signing; no keystore should be placed in
  the repository.
  Normal experimental APKs enforce Free/Pro, but cannot buy/restore Pro without a configured
  license public key and production billing. They must not be marketed as the purchasable Play app.
  The guarded Play bundle workflow is the path for the stable store identity and trust.
- Linux is an unsigned portable archive, not a distro-native package. Distribution coverage and a
  signing mechanism remain owner approval gates.
- Camera, display, cross-device, and upgrade behavior still require the platform validation matrix
  described in `WebAndMarketingPlan.md`. Release publication does not turn an unverified platform
  into a Stable one.

The workflow deliberately does not pretend to provide certificate signing, notarization, installer
reputation, package-manager publication, Play Store publication, or reproducible builds. Add those
only after the owner approves the corresponding credentials, operational process, and release gate.
