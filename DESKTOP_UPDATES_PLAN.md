# Desktop update plan

Status: proposed architecture and delivery plan, 2026-10-25. Implementation has not started.

## Intended experience

FileFlow handles downloading, verifying, installing, and relaunching desktop updates. Users do not
visit a download page or rerun an installer for routine updates. Automatic mode stages an update
and installs it when FileFlow next closes safely. With automatic installation off, a quiet in-app
notification offers **Update and restart**, **Install when I close FileFlow**, and **Remind me later**.

Android and future iOS distribution use their app stores for updates. Do not add this desktop updater
to the mobile apps. The browser app's service worker and the website's Docker updater remain separate.

Existing desktop v0.2.0 builds do not contain an updater. Their users need one final manual upgrade
to the first updater-enabled release. A new GitHub asset or DMG cannot retrofit update code into an
already installed executable. All subsequent supported desktop upgrades should happen inside the app.

The updater necessarily downloads software internally. Initially prefer a verified full package;
delta downloads are an optimization after reliability is established.

## Current foundation and gaps

The desktop is a native C++20/SDL3 app, built by `desktop/CMakeLists.txt`. Its event/render loop is in
`desktop/src/main.cpp`. `Settings::Load/Save`, recent files, and desktop team data use the stable
`SDL_GetPrefPath("FileFlow", "Desktop")` location through `PrefPath` in `desktop/src/app.cpp`.
There is no existing update controller, update preference, notification flow, or installation helper.

Owner-approved milestones already publish reviewed builds through `.github/workflows/milestone-release.yml`.
The website verifies their release manifest, checksums, and GitHub asset digests. Current desktop
downloads are a Mac DMG, Windows portable ZIP, and Linux portable tarball. Mac app signatures are
ad-hoc; Windows and Linux packages do not have production publisher signatures.

Keep that publication model. Add authenticated updater metadata and platform update payloads to it.
The current website `releases.json` is a download catalog, not sufficient authorization to execute
downloaded code: a checksum served alongside a replaced payload does not establish publisher trust.

## Platform approach

| Distribution | Proposed implementation | First installation | Routine updates |
| --- | --- | --- | --- |
| macOS direct download | Sparkle 2, integrated through a small Objective-C++ bridge | Keep the branded DMG and drag to Applications | Signed update archive, downloaded by Sparkle; bundle replacement on safe quit/restart |
| Windows direct download | Velopack native C/C++ integration, subject to the security and recovery proof below | One-click, signed, per-user setup | Managed update packages and helper; no installer wizard on routine updates |
| Linux direct download | Velopack native integration with a self-updating AppImage, subject to the same proof | AppImage at a stable user-writable location, with optional launcher integration | Verified replacement through its updater; preserve the launcher path |
| Linux package-manager installation | The owning package manager | Flatpak, deb/rpm, or another explicitly supported package route | Manager-owned updates; FileFlow does not overwrite managed files |
| Any future desktop store build | Its store | Store installation | Store-owned updates; no competing in-app binary updater |

Sparkle documents signed update archives, automatic installation behavior, and non-Xcode embedding.
Keep the DMG for acquisition and recovery; use the archive format recommended by the pinned Sparkle
release for in-app delivery. It can differ from the website download. [Sparkle setup](https://sparkle-project.org/documentation/),
[automatic-update customization](https://sparkle-project.org/documentation/customization/).

Velopack has native C/C++ bindings with separate check, download, and apply operations. Its early
startup integration must be isolated from normal app startup and shutdown. Its packaging CLI needs
.NET in CI; FileFlow's C++ application does not become a .NET application.
[Native integration](https://docs.velopack.io/getting-started/cpp).

On Windows, its per-user layout provides a stable launcher outside the replaceable application
directory. Keep preferences outside that directory. Do not request system-wide installation by
default. [Windows layout](https://docs.velopack.io/packaging/operating-systems/windows).

On Linux, Velopack packages AppImages and replaces them during updates. Privileged locations can
require authorization, so use a user-owned stable path for the recommended download. AppImage
packaging alone does not prove compatibility with older glibc, graphics stacks, or cameras.
[Linux behavior](https://docs.velopack.io/packaging/operating-systems/linux).

Velopack is a provisional selection, not an unconditional security approval. Its current C++ SDK
must prove that FileFlow can validate the exact downloaded target before any apply path runs,
including pending-update startup paths. Disable default startup auto-apply until our policy has
authorized a verified update. If this cannot be demonstrated, do not ship that integration; revisit
the platform backend. [Startup contract](https://github.com/velopack/velopack.docs/blob/master/docs/integrating/overview.mdx).

AppImageUpdate is an alternative worth testing if the Linux Velopack adapter fails that gate. Its
delta-update support does not by itself establish publisher authenticity; any alternative must pass
the same signature and replacement tests. [AppImage update guidance](https://docs.appimage.org/packaging-guide/optional/updates.html).

## Preferences and notifications

Proposed first-run choice: recommend automatic updates, explain the online check once, and let the
user choose notifications only. Until a choice is made, do not silently enable binary installation.

| Setting | Behavior |
| --- | --- |
| Automatically install updates | Enables checks and background staging; installs on a safe quit; never forces a running transfer to stop |
| Notify me about updates | Checks while the app runs and shows an update banner; no unattended installation |
| Fully offline | No scheduled update network requests; manual Check for updates asks to go online |
| Check for updates | Available in About/Settings; immediate, user-requested check with a useful result |
| Channel | Stable by default; explicit opt-in for Beta |

Turning automatic installation off must leave update notifications on. Turning all network checks
off is a separate, clearly named choice. If the user disables auto-install after a download, retain
the verified package but cancel unattended application of it, including on the next launch.

Check after the UI opens, then roughly every six hours while running, with jitter and exponential
backoff. Debounce repeated manual clicks. Do not block startup, capture, decoding, or rendering on
network operations. Show the last successful check; distinguish **Offline**, **Check failed**, and
**Up to date**. A failed request is not evidence that the user has the latest version.

Use one nonmodal banner per available version, with a persistent About/Settings badge and a default
24-hour reminder. Optional OS notifications respect system permissions and Do Not Disturb. In-app
notification remains available if system notifications are denied. Do not put a modal over an optical
pattern or steal focus from a presentation.

The initial implementation checks while FileFlow runs and on the next launch. It does not install
an always-running service merely to notify users while the app is closed. If closed-app checks are
later required, evaluate an opt-in, OS-managed scheduled task as a separate feature.

## Shared controller and safe application boundary

Keep a small shared update policy/controller in the desktop shell, with platform backends handling
OS installation. Do not put network/update code in portable `core/` or the mobile transport library.

Proposed integration points:

- `desktop/src/updates/`: preferences, version/channel selection, state machine, notification policy,
  journal, and backend interface.
- `desktop/platform/macos/`: Sparkle Objective-C++ adapter and main-thread callbacks.
- `desktop/platform/windows/` and `desktop/platform/linux/`: native backend adapters.
- `desktop/src/main.cpp`: early framework hooks, lifecycle coordination, and graceful exit.
- `desktop/src/app.*`: About/Settings controls, banners, and transfer-busy status.
- `desktop/CMakeLists.txt` and platform packaging: pinned dependencies, helper inclusion, signing.

State model:

```text
Idle → Checking → Available → Downloading → Verifying → Ready
                                      failure ↘ Retry later
Ready → Waiting for safe quit → Applying → Relaunching → Healthy
                                             failure ↘ Recovery
```

Network and expensive verification run off the SDL thread. Marshal status changes back through the
app's event mechanism. Throttle or pause staging during optical work if it affects frame timing.

Before applying, require all of the following:

1. No active sender, presenter, camera receive, encryption/pairing operation, file-save dialog, or
   uncommitted local write. A rateless sender remains busy until the user stops it; completing one
   display pass is not permission to restart.
2. The target is verified, compatible, still authorized for this channel, and allowed by current
   preferences. Recheck a withdrawn release before applying when online; defer unattended apply
   when fresh authorization cannot be established.
3. Settings and recent state are atomically persisted, worker threads/camera handles are closed,
   and the backend is instructed to wait for graceful exit.
4. Only after the process has exited does the platform helper replace program files and relaunch
   once. A shutdown timeout defers the update; it does not kill a transfer to meet a deadline.

When a user clicks Update during a transfer, offer **Install after transfer**. A separate explicit
stop-and-update action must explain that an unfinished receive loses its current progress. Never
claim that in-memory optical receive state can resume across an application restart today.

## Trust and release delivery

Use the existing GitHub Releases as immutable payload storage and the existing Linux/Caddy site for
small update feeds. Desktop clients fetch static metadata; they do not each poll the GitHub Releases
API or use the account database. An unavailable website, GitHub, or account service must not stop
ordinary offline transfer use.

Create a dedicated signed updater catalog rather than extending the public website catalog into an
execution trust root. Prefer an established TUF implementation for metadata trust, expiration, and
rollback protection, integrated with the native backends. Do not implement a home-grown TUF clone.
The implementation spike must identify and exercise an appropriate maintained client/library before
production auto-install is approved. [TUF overview](https://theupdateframework.io/docs/overview/),
[threat model](https://theupdateframework.io/docs/security/).

Required properties, regardless of the selected library:

- Embed trusted public keys in the first updater-enabled app. Keep private update-signing keys
  separate from hosting credentials and Apple/Windows code-signing identities.
- Authenticate channel, version, monotonic metadata sequence, platform, architecture, minimum OS,
  package type, exact size and SHA-256, and data-format compatibility. Verify payload signatures
  and OS publisher identity as applicable. HTTPS and checksums alone are not the trust decision.
- Verify the final reconstructed full payload even when a delta was used. Fail closed for bad
  signatures, expired metadata, unexpected redirects, incorrect hashes, or incompatible targets.
- Persist trusted metadata versions to reject replay/downgrade. Test planned key rotation and
  recovery, including a client returning after a long offline period. If metadata expires, keep
  the current app usable and defer updates.
- Keep update files in a private cache, lock concurrent attempts, and prevent archive traversal,
  symlink escapes, arbitrary destination writes, command injection, and unsigned helper execution.
- Explicitly handle any framework auto-apply/default hooks so they cannot bypass verification,
  user preferences, or the active-transfer gate. This is a release blocker, not a later hardening task.

Use native Sparkle archive signatures as an additional Mac requirement. Sign the complete Mac app
and its helpers with Developer ID, notarize and staple the distribution artifacts. On Windows,
sign the app, setup, and updater/helper binaries with a consistent Authenticode publisher; verify the
chosen framework's packaging/signing order. Do not promise that signing instantly eliminates all
SmartScreen warnings. [Velopack signing order](https://docs.velopack.io/packaging/signing).

There is no automatic installation from the current unsigned/ad-hoc public experimental packages.
Development-key experiments are confined to a separate test feed and disposable installations.

## Owner-controlled publication and rollout

Extend the current milestone workflow rather than publishing on every push:

1. Resolve the exact owner-reviewed source commit and run platform tests.
2. Build and sign the first-install packages and separate updater payloads. Keep DMG, managed
   Windows setup, and AppImage available as normal acquisition/recovery downloads.
3. Verify packages on supported OS/architecture combinations, including an N → N+1 upgrade from a
   real installed build. Generate framework feeds and authenticated metadata from final package bytes.
4. Upload immutable payloads to a draft Release, verify downloaded bytes, then publish the Release.
   Only after publication, verify canonical asset URLs; GitHub's draft URLs are not stable.
5. Publish the signed update-feed snapshot and atomically promote the channel pointer last. Verify
   through the public endpoint before marking the release complete. Until that final promotion,
   clients keep the previous trusted target.
6. Begin with an internal/canary channel, then a small stable cohort, then wider rollout after review.
   Cohort selection can be local and random; do not transmit a persistent device identifier for it.

Suggested public feed base: `https://fileflow.anomalousinteractive.com/updates/`, separated by
channel/platform/architecture. Actual feed names/formats depend on the pinned frameworks. Use short
metadata cache lifetimes (about five minutes) with revalidation, long immutable payload caching, and
bounded retry behavior. Show available/installed version and check time in the app.

If using expiring signed metadata, schedule narrowly scoped freshness renewal even during weeks
without a release. That renewal must not authorize a new application target or replace the owner's
milestone approval. Key availability, expiry monitoring, and recovery belong in the release runbook.

Withdraw a bad release by stopping channel promotion and publishing authenticated replacement
metadata. Never replace bytes behind an already published package URL. For a fleet rollback, prefer
a new, higher-version release containing the previously good code. Keep installer links and updater
feeds generated from the same approved release record so they cannot drift independently.

## Data preservation and recovery

Preserve the existing `PrefPath` identity and location. Keep `settings.txt`, `recent.txt`, `teams.txt`,
received files, saved destinations, pairing/team keys, and future offline license state outside
replaceable program directories. Do not move them into a Velopack `current` directory or the Mac
app bundle. Desktop updates do not migrate or restore the website's account database.

Make preference writes atomic before enabling automatic updates. Add a versioned update-preference
file and private installation journal without changing existing settings silently. Future user-data
migrations must be transactional or backed up, with declared read/write compatibility.

Retain one verified previous application version until the replacement passes a defined startup
health check. The check should cover preferences/key-store readability and successful application
initialization; absence of a camera or an offline network is not an update failure. A small launcher
or the platform framework must be able to recover before a crashing app's normal UI runs.

Do not assume a framework supplies crash-loop recovery automatically. Prove this per platform.
If the new binary cannot start and the previous binary can still read the current data, offer or
perform the narrowly authorized local binary rollback recorded in the installation journal. If data
is no longer backward-compatible, preserve it and enter recovery; never silently restore an older
data snapshot over newly received files or changed keys. Ordinary feed downgrade protection remains
enabled; this local recovery path is restricted to the recorded, verified prior installation.

Handle full disk, interrupted downloads, power loss during replacement, another app instance,
antivirus/file locks, a relocated AppImage, read-only media, and insufficient write permissions.
Do not install an elevated background service merely to hide authorization prompts. Managed or
administrator-owned installations may still need OS authorization or IT deployment. That is an
explicit limit to the no-prompts goal.

## Licensing, privacy, and distribution ownership

Updates remain available without signing into a FileFlow account. Metadata checks do not include
filenames, camera images, team names, keys, transfer history, or account identifiers. Minimal hosting
access logs still exist; document their retention without labeling update requests as telemetry-free
network traffic. Routine update failures stay in bounded local diagnostic logs unless shared by the user.

For future commercial plans, separate update availability from a signed license's upgrade eligibility.
Never silently install a paid major version that removes currently licensed use. Clearly offer an
optional major upgrade while continuing the compatible supported branch/security policy. Define that
policy before shipping paid plans; this plan does not set pricing or license terms.

Detect installation ownership. For Flatpak/package-manager/store builds, show **Updates managed by
your software store** and use that manager's supported update path. OS policy may control its timing
and notifications. Keep the in-app replacement backend disabled there. Advanced source/tarball builds
can provide notices, but must not be advertised as seamless self-updating downloads.

## Delivery phases and acceptance gates

| Phase | Deliverable | Exit criteria |
| --- | --- | --- |
| 0 — Prove the mechanisms | Pin licenses/versions; C++ integration spikes for Sparkle and Velopack; choose authenticated feed library; define supported OS/architecture matrix and signing custody | On each platform, a real A → B update is verified, applies without a routine installer wizard, preserves data, and obeys auto-off/transfer-busy rules. Framework startup paths cannot bypass trust. Record actual permission prompts and recovery limitations. |
| 1 — Shared foundation | Settings/notification UI, controller/state machine, install ownership detection, background checks, signed test feed, atomic preference writes | Tests cover every mode, offline operation, wrong target, stale/bad signatures, cancellation, and UI responsiveness. No production update target is published. |
| 2 — macOS | Sparkle adapter, production signing/notarization, DMG plus update archives, lifecycle and recovery integration | Real drag-installed app upgrades from A to B and A to C; read-only DMG execution and non-admin locations are handled clearly; active transfer is never stopped automatically. |
| 3 — Windows and Linux | Per-user managed Windows installation and AppImage packaging/adapters | Updates preserve launchers, preferences and keys; locked files/low disk/power-loss tests pass; Linux is tested on the declared oldest supported userspace, X11/Wayland and representative cameras. |
| 4 — Bootstrap and canary | Owner-approved first updater-enabled milestone and one-time v0.2.0 migration instructions | Legacy users keep existing state; canary users complete at least two consecutive upgrades; rollback/withdrawal/key-rotation exercises pass before stable rollout. |
| 5 — Optimize | Optional deltas, additional architectures and package/store channels | Full-download fallback remains reliable; no extra installation friction or weaker verification. |

Automatic updates are ready only when these scenarios pass on every advertised platform:

- Auto on: a verified release downloads and installs on safe exit without opening a browser or wizard.
- Auto off: a notice appears; no download/application happens without the configured permission;
  Update and restart completes the upgrade with one user action when idle.
- Fully offline: no scheduled network traffic; send/receive and existing licensing remain usable.
- A long-running sender, receiver, presenter, or pending file save survives update availability unchanged.
- Failed download, corrupt payload, forged feed, wrong architecture, expired metadata, withdrawal,
  interrupted replacement, and failed startup leave a usable app or a data-preserving recovery path.
- Settings, recent files, keys and received files survive A → B, A → C, and an allowed local rollback.
- No-update checks remain cheap and bounded under a large fleet; no per-client account DB traffic.

## Decisions and spending before implementation

The baseline is free, open-source updater software on our existing infrastructure. Sparkle's main
license is MIT with additional bundled notices; Velopack is MIT. Neither requires a paid updater
subscription for this self-hosted design. Keep the required license notices with the distribution.
[Sparkle license](https://github.com/sparkle-project/Sparkle/blob/2.x/LICENSE),
[Velopack license](https://github.com/velopack/velopack/blob/develop/LICENSE).

Generating our own update-authentication keys and signing update metadata has no certificate-vendor
fee. OS-recognized publisher identity is a separate concern. Apple's standard Developer Program
membership is currently USD 99/year and provides access to Developer ID; an existing membership
used for iOS distribution can also cover this work. Windows signing may qualify for free open-source
signing through SignPath Foundation, subject to eligibility and approval. Linux has no mandatory
commercial publisher-certificate fee for our proposed signed AppImage update channel.
[Apple membership](https://developer.apple.com/programs/enroll/),
[Developer ID](https://developer.apple.com/developer-id/), [SignPath Foundation](https://signpath.org/).

Start development and disposable test updates without buying a service. Hosting, bandwidth, CI
allowances, maintenance, and any production signing identity still need budgeting. A wholly custom
updater is possible, but would make us responsible for secure replacement, verification, recovery,
and OS integration. Using these open-source components keeps our feeds, releases, and user experience
under our control while reusing maintained installation machinery.

Approve the first-run default, support policy, and platform/backend choices after the spikes. Select
Apple and Windows signing identities, key custody/recovery, and supported machine architectures.
Obtain current signing/provider quotes before any purchase. No paid updater hosting is required by
this proposal: start with existing GitHub Releases and Linux/Caddy infrastructure, then measure load.

Actual milestone versions, publication, credentials, paid services, and store enrollment remain owner
decisions. Planning this work does not enable updates, change app behavior, or publish another release.
