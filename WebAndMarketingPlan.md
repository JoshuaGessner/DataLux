# DataLux — Cross-Platform Website, Web App & Marketing Master Plan

> **Status:** Proposed planning snapshot; current commercial policy in PRODUCT.md
> **Owner:** Product / web
> **Last reviewed:** 2026-10-28
> **Related:** PRODUCT.md, NATIVE_RELEASES.md

**Repository:** https://github.com/JoshuaGessner/DataLux
**Date:** October 25, 2026  
**Status:** Active development and platform testing  
**Purpose:** Complete the public website, browser application, cross-platform distribution, branding, documentation, and marketing launch infrastructure.

**Planning snapshot:** This document preserves the original October 25 direction, including future
work. It is not a release status report. For current distribution and account behavior, use
[NATIVE_RELEASES.md](NATIVE_RELEASES.md), [desktop/ACCOUNTS.md](desktop/ACCOUNTS.md), and the separate
[website repository](https://github.com/JoshuaGessner/DataLux-Web). Desktop automatic updates are
still proposed in [DESKTOP_UPDATES_PLAN.md](DESKTOP_UPDATES_PLAN.md); paid sales remain disabled.

**October 28 decision:** [PRODUCT.md](PRODUCT.md) is the current commercial policy: every desktop
feature is free; Android Free retains single-file transfers and security; $10 USD one-time Android
Pro adds batch sending/receiving and bulk file actions, with an initial US Play launch. Older paid
desktop proposals below are superseded. Google Play production approval and real purchase tests
remain release gates.

---

# 1. Critical Project Context

DataLux is no longer an Android-only application.

A native cross-platform desktop suite has already been implemented and is currently undergoing testing.

The project includes:

- Native Android application.
- Native macOS desktop application.
- Native Windows desktop application.
- Native Linux desktop application.
- Shared C++ optical transfer technology.
- Cross-device encryption and pairing.
- Encrypted team-based transfers.
- Multi-display and projector transmission.
- Batch transfers and desktop productivity features.

**Do not plan or implement a new desktop application from scratch.**

Do not recommend Electron, Tauri, or another desktop framework as a replacement for the existing suite without explicit authorization.

The existing desktop implementation uses C++20, SDL3, and shared native components. Preserve this architecture.

The primary new development objectives are:

1. A professional public website.
2. A functional browser-based version of DataLux.
3. Cross-platform download and release infrastructure.
4. Product branding and marketing assets.
5. Press and technical documentation.
6. A carefully staged public launch.
7. A monetization pathway that preserves the open-source project's credibility.

The website and web app must complement the existing desktop and Android software.

---

# 2. Product Positioning

## Primary identity

**DataLux — Send files with light.**

Suggested positioning:

"Transfer files directly between phones and computers using their screens and cameras. No Wi-Fi, Bluetooth, cables, or internet connection required."

Emphasize that DataLux is a cross-platform optical communication system.

Do not position the desktop software as an upcoming capability.

## Main differentiators

### Cross-platform optical transfer

Native applications target Android, Windows, macOS, and Linux.

A browser implementation is in development to expand accessibility without requiring native installation.

### Offline operation

Data travels through a visible optical signal rather than a conventional network connection.

The actual optical transfer does not require cloud servers.

### Multi-receiver broadcasting

A single display can transmit an optical data stream to multiple receivers.

This is a major marketing opportunity.

The desktop application includes support for external monitors, televisions, and projectors.

Distinguish the implemented capability from the degree of real-world multi-receiver testing.

### Encrypted transfers

Existing implementations include encryption and device pairing.

Security messaging must accurately describe authenticated pairing, encryption, group-key limitations, and physical observation risks.

### Encrypted teams

A sender can distribute encrypted content to an authorized team.

Do not imply perfect access revocation, invisible transmissions, or independently audited security.

### Desktop productivity

The desktop suite includes:

- Drag-and-drop file selection.
- Multiple-file sending.
- Clipboard text transmission.
- Recent sends.
- Configurable speed modes.
- Compression settings.
- Camera-based receiving.
- Presenter view on a secondary display.
- Command-line launch options.
- Encrypted pairing and team functionality.

Treat these as implemented features requiring platform-specific validation.

---

# 3. Initial Codebase Audit

Before making changes, pull the current repository and record its exact commit SHA.

Inspect:

- `desktop/`
- `core/`
- `app/`
- `platform/android/`
- `sim/`
- `fuzz/`
- `harness/`
- `cmake/`
- `.github/workflows/`
- `CMakeLists.txt`
- `CMakePresets.json`
- Gradle configuration.
- Licensing and security documentation.

Create:

`docs/PLATFORM_STATUS.md`

Document the actual state of:

- Android builds.
- Windows builds.
- macOS builds.
- Linux builds.
- Desktop-to-Android compatibility.
- Android-to-desktop compatibility.
- Desktop-to-desktop compatibility.
- Multi-receiver broadcasting.
- Encrypted pairing.
- Teams.
- Performance on supported configurations.
- Distribution readiness.

Use the following status classifications:

- Implemented, validated.
- Implemented, testing.
- Experimental.
- Planned.
- Unsupported.

**Never confuse implementation with verified compatibility.**

The current public README reports successful macOS testing, Linux builds and tests, and pending Windows and end-to-end webcam validation. Reconcile that documentation with the latest development progress.

The final website's feature matrix must come from verified platform results.

---

# 4. Website Architecture

The website should become the central hub for the entire DataLux ecosystem.

It should introduce the technology, demonstrate the product, distribute native applications, launch the web app, and support marketing.

## Recommended technology

Use Astro or an equivalent static-first framework with TypeScript.

Keep the marketing website lightweight.

Use a separate application bundle for the browser transfer interface if necessary.

Avoid introducing a dependency-heavy backend for functionality that should operate locally.

Suggested navigation:

- Home
- Download
- Web App
- How It Works
- Features
- Benchmarks
- Documentation
- Blog
- GitHub

Additional pages:

- Security.
- Privacy.
- Commercial Licensing.
- Press Kit.
- Support.
- Changelog.

## Home page

Hero:

**Send files with light.**

Subtitle:

"Move files between your phone and computer using only a screen and camera. No Wi-Fi, Bluetooth, cables, or internet."

Primary CTA:

**Download DataLux**

Secondary CTA:

**Try in Browser**

The browser CTA should be labeled Preview or Beta until the browser app is ready for general release.

Below the hero, include a real demonstration of DataLux communicating between different device types.

Suggested feature sections:

1. Phone-to-phone transfer.
2. Computer-to-phone transfer.
3. Phone-to-computer transfer.
4. One screen, multiple receivers.
5. Encrypted transfers and teams.
6. No network required.
7. Open-source engineering.

Show genuine application screenshots rather than conceptual mockups once screenshots become available.

---

# 5. Cross-Platform Download Experience

The Download page is now one of the highest-priority website components.

Provide platform-specific distribution:

**Android**

- Google Play when published.
- GitHub release APK where available.
- Supported Android versions.
- Installation instructions.

**Windows**

- Native Windows application.
- Verified installer or portable package.
- Supported architectures.
- Signed-build status and Windows security guidance.

**macOS**

- Native macOS application.
- Supported Apple Silicon and Intel configurations, as confirmed.
- DMG or other appropriate distribution.
- Code-signing and notarization status.

**Linux**

- Native Linux application.
- Supported distributions and architectures.
- Available binary packages or installers.
- Installation guidance and dependencies.

**Web**

- Browser-based application when functional.
- Supported browsers.
- Camera permission requirements.
- Known performance limitations.

Each platform should have a status indicator:

- Stable.
- Beta.
- Experimental.
- Coming soon.

Do not automatically mark a platform Stable simply because its source code compiles.

Implement OS-aware recommendations but retain manual platform selection.

Each release should have:

- Version number.
- Publication date.
- Release notes.
- Architecture.
- Verified download link.
- SHA-256 checksum.
- Known issues.

Investigate GitHub Releases as the initial binary distribution backend.

Do not invent download URLs or serve unverified binaries.

---

# 6. Browser Application

The browser application remains the major new engineering task.

However, unlike the previous plan, this project already has two mature native implementations to use as references.

## 6.1 Reuse the native core

Investigate compiling relevant portions of the existing C++ core to WebAssembly.

Use the native Android and desktop implementations as compatibility references.

Do not create a separate incompatible optical format for the browser.

Prefer shared:

- Framing.
- Payload encoding.
- Error correction.
- Compression.
- Integrity verification.
- Encryption formats.
- Transfer metadata.
- Protocol versioning.

Where native OS interfaces cannot be reused, create browser-specific adapters.

## 6.2 Browser sender

Support:

- File selection.
- Drag-and-drop.
- Multiple-file sending where feasible.
- Full-screen optical transmission.
- Speed selection.
- Compression selection.
- Progress and cancellation.
- Device-specific performance warnings.

Consider browser refresh rates and rendering throttling.

Do not assume that JavaScript rendering can match SDL3/native performance.

Implement adaptive transmission settings based on measured capability.

## 6.3 Browser receiver

Support:

- Camera permission handling.
- Camera selection.
- Optical frame detection.
- Shared-protocol decoding.
- Error recovery.
- Integrity verification.
- File download.
- Progress and troubleshooting feedback.

Explore efficient video-frame access, Web Workers, and GPU processing where beneficial.

Avoid unnecessarily copying camera frames between threads or repeatedly allocating buffers.

## 6.4 Interoperability targets

Test:

- Browser to Android.
- Android to browser.
- Browser to native desktop.
- Native desktop to browser.
- Browser to browser.

Create golden-vector tests comparing WebAssembly and native protocol outputs.

Encryption and pairing must remain compatible.

Never silently downgrade encrypted sessions.

## 6.5 Browser distribution

Provide the web application through HTTPS.

Consider PWA installation and offline caching.

The browser application's core file-transfer functionality should not require a server after assets have loaded.

Do not claim fully offline browser operation until verified with the network disabled.

---

# 7. Desktop Suite: Validation and Distribution, Not Redevelopment

The desktop suite is a core existing product.

The new agent's responsibilities are limited to integration, packaging, documentation, testing infrastructure, and launch preparation unless additional desktop work is explicitly assigned.

## Validate the existing capabilities

Check:

- Native window behavior.
- Full-screen transmission.
- Multi-monitor selection.
- Projector transmission.
- Camera initialization.
- Drag-and-drop.
- Batch file handling.
- Clipboard transmission.
- Compression.
- Speed modes.
- Encryption.
- Pairing.
- Teams.
- Received-file storage.
- Command-line options.

Test meaningful combinations of operating systems and receiving devices.

Create a release-readiness matrix.

## Distribution improvements

Investigate:

- Automated Windows builds.
- Automated macOS builds.
- Automated Linux builds.
- Code signing.
- macOS notarization.
- Reproducible release artifacts.
- Checksums.
- Signed updates.
- Package manager distribution where appropriate.

Microsoft Store, Apple distribution, Winget, Homebrew, and Linux package repositories are possible future distribution channels.

Prioritize direct verified downloads first.

Do not replace the native desktop architecture.

---

# 8. Primary Marketing Demonstrations

The desktop suite creates significantly better marketing opportunities than the previous Android-only plan.

Develop three primary demonstrations.

## Demonstration A — Phone to computer

Show an Android phone transmitting a file to a computer webcam.

Display the completed file opening on the computer.

Use real measured performance and supported settings.

## Demonstration B — Computer to phone

Drag a file into DataLux on a computer.

Show the optical pattern filling the computer monitor.

Capture a phone receiving and opening the file.

This is a strong short-form demonstration because it requires very little explanation.

## Demonstration C — One screen, multiple receivers

Use a monitor, television, or projector as the transmitting surface.

Show multiple phones receiving the same file.

Where feasible, demonstrate independent receiver progress and successful integrity verification.

This may be the strongest marketing asset for DataLux.

Do not publish it as a reliable supported workflow until tested successfully.

---

# 9. Updated Marketing Positioning

The audience is now broader than Android enthusiasts.

Priority audiences:

1. General technology enthusiasts.
2. Open-source and software developers.
3. Desktop utility users.
4. Makers and engineering communities.
5. Educators and workshop organizers.
6. Offline-computing enthusiasts.
7. Organizations evaluating specialized optical communication technology.

The desktop suite makes DataLux more interesting to audiences who primarily use Windows, macOS, or Linux.

## Messaging examples

**Short:**

"DataLux transfers files through light instead of wireless networking."

**Consumer:**

"Move files between your phone and computer without Wi-Fi, Bluetooth, cables, or the internet."

**Technical:**

"An open-source, cross-platform optical file-transfer system that encodes data into display frames and reconstructs it using a camera."

**Broadcast:**

"One screen can transmit the same file to multiple compatible receivers."

**Security:**

"Optional encrypted transfers and authenticated pairing for supported devices."

All public copy must match the actual release's verified capabilities.

---

# 10. Media and Press Strategy

The expanded desktop suite strengthens the editorial story.

Previously, DataLux was primarily an unusual Android experiment.

Now the story is a cross-platform optical transfer system with native applications, encrypted groups, and potential one-to-many distribution.

Prioritize:

- Hackaday.
- XDA Developers.
- Android Authority.
- Ars Technica.
- Relevant YouTube technology channels.
- Open-source engineering publications.
- Developer-focused communities.

Prepare:

- A working release.
- A press-friendly technical summary.
- A 30-second demonstration.
- An extended engineering video.
- Authentic application screenshots.
- A reproducible benchmark report.
- A supported-platform matrix.
- A press contact.
- A clear security and privacy explanation.

## Suggested press angle

"I built a cross-platform file-transfer system that uses screens and cameras instead of wireless networking."

Supporting points:

- Native Android and desktop applications.
- Optical transfer through display and camera.
- Offline operation.
- Encrypted communication.
- Team-based transfer.
- Multi-receiver broadcast capability.
- Open-source implementation.

Avoid claims that DataLux invented optical file transfer or is definitively faster than existing solutions.

Do not initiate unsolicited press campaigns without project-owner approval.

---

# 11. Monetization

Do not rush subscription billing.

Preserve the free open-source core.

Three monetization pathways are worth evaluating.

## Free consumer application

Offer standard optical transfer functionality without payment.

Use free distribution to build adoption and demonstrate the technology.

## Android Pro convenience workflows (updated October 28)

Desktop productivity remains free. Android's one-time Pro upgrade sells repeat-work convenience
while preserving full-speed single-file transfers, encryption, pairing and teams in Free.

The launch feature list is:

- Multi-file batch sending on Android.
- Multi-file batch receiving on Android, including batches sent by the free desktop.
- Selecting, sharing, resending and deleting several received files together.

Folder recursion, scheduling and automation are future ideas, not included launch promises.

The approved US launch price is $10 USD one time through Google Play product `fileflow_pro`.
The final checkout price comes from Google Play. See [PRODUCT.md](PRODUCT.md) for account restoration,
receiving-device enforcement, offline limits and the US-first rollout.

## Commercial SDK licensing

This is potentially the strongest longer-term revenue pathway.

Investigate commercial licensing of the native optical transfer core, integration libraries, and engineering support.

Prepare a licensing page that invites qualified commercial inquiries.

The existing AGPL-3.0 and dual-licensing structure requires review of copyright ownership, dependencies, and contributor permissions before commercial licensing terms are finalized.

Do not claim an SDK product is available until it is packaged, documented, and supported.

---

# 12. Content Marketing

Publish original technical and product content.

Suggested launch articles:

1. **"I Built a Cross-Platform App That Sends Files Using Light"**
2. **"How DataLux Transfers Files Between Computers and Phones Without Wi-Fi"**
3. **"One Screen, Multiple Receivers: Optical File Broadcasting"**
4. **"Why We Built DataLux's Desktop Suite Natively"**
5. **"Turning a Native C++ Optical Protocol Into a Browser Application"**
6. **"Benchmarking Optical File Transfer on Real Hardware"**
7. **"How DataLux Handles Encryption, Pairing, and Team Transfers"**

Use genuine measurements, screenshots, diagrams, and test footage.

The team-based broadcasting article may become particularly valuable because it demonstrates a use case distinct from ordinary peer-to-peer file sharing.

---

# 13. Launch Phases

## Phase 1 — Consolidation

Audit the current Android and desktop implementations.

Establish platform status and testing results.

Deliver:

- Current source audit.
- Feature and platform matrix.
- Known-issues list.
- Versioning strategy.
- Packaging recommendations.
- Marketing claims register.

## Phase 2 — Website and native downloads

Build the website while packaging the desktop applications and Android app for external beta testers.

Deliver:

- Website MVP.
- Cross-platform downloads.
- Platform requirements.
- Documentation.
- Privacy/security pages.
- Real demo content.

**This phase should not wait for the browser app.**

## Phase 3 — Public native beta

Recruit testers across Android, Windows, macOS, and Linux.

Validate actual interoperability and usability.

Gather compatibility results and fix critical transfer issues.

Use a limited, honest beta announcement rather than a broad product launch.

## Phase 4 — Web implementation

Port the necessary core functionality to the browser.

Test against the existing native implementations.

Release as a preview when reliable.

The native product should continue improving independently.

## Phase 5 — Larger marketing launch

Once external users can install the native applications reliably:

- Publish major demonstration videos.
- Publish benchmark results.
- Release detailed engineering articles.
- Submit appropriate community announcements.
- Begin selective press outreach.
- Make the browser preview available if sufficiently tested.

Do not delay a strong native release solely to wait for browser parity.

## Phase 6 — Monetization validation

Measure adoption and real feature demand.

Determine whether commercial interest concentrates on:

- Desktop productivity.
- Cross-platform convenience.
- One-to-many broadcasting.
- Developer integration.
- Commercial SDK licensing.

Build paid offerings based on demonstrated demand.

---

# 14. Success Metrics

Initial success should be evaluated through:

- Verified release downloads.
- Unique beta testers, where measurable without invasive tracking.
- Successful first transfers.
- Platform compatibility.
- Transfer reliability.
- Support reports.
- Organic community interest.
- Demonstration-video engagement.
- Press mentions.
- Inbound commercial licensing inquiries.

Do not treat GitHub stars or video impressions as substitutes for reliable product use.

Use privacy-respecting website analytics.

Do not collect transferred file contents, camera frames, filenames, or encryption keys for marketing purposes.

---

# 15. Agent Execution Rules

The agent must:

1. Treat the desktop suite as implemented and under test.
2. Preserve the existing desktop and Android applications.
3. Reuse the shared C++ protocol whenever practical.
4. Avoid duplicating existing native functionality.
5. Separate confirmed features from experimental ones.
6. Never fabricate performance data.
7. Keep product copy aligned with testing.
8. Build the website independently of browser-app readiness.
9. Prioritize verified native downloads.
10. Make the web app interoperable rather than inventing a disconnected protocol.
11. Respect open-source licensing and privacy.
12. Avoid unnecessary analytics or advertising infrastructure.
13. Use actual product screenshots and recordings.
14. Keep changes incremental, reviewable, and reversible.
15. Require owner approval before publication, paid spending, external outreach, or license changes.

---

# 16. Immediate Deliverables

The agent should begin with the following deliverables.

**Deliverable A — Platform Audit**

Create `docs/PLATFORM_STATUS.md` using the latest source and test results.

**Deliverable B — Website Architecture**

Create the website scaffold, shared design system, navigation, and initial home/download pages.

**Deliverable C — Release Infrastructure**

Prepare a release strategy for native desktop installers and Android distribution, including signing and artifact verification.

**Deliverable D — Web Feasibility**

Identify portable components and demonstrate an initial browser-to-native optical protocol exchange.

**Deliverable E — Marketing Foundation**

Prepare the product messaging guide, demonstration shot list, media kit structure, content calendar, and accurate feature claims.

**Deliverable F — Implementation Roadmap**

Organize remaining work by dependencies, acceptance criteria, and test requirements.

Work on the website, native packaging, and web feasibility in parallel where dependencies allow.

---

# 17. Final Strategic Direction

DataLux should launch as a cross-platform optical communication product, not as a mobile experiment.

The native desktop suite is a major part of its current value and should be featured prominently.

The browser application is an accessibility and adoption enhancement, not a prerequisite for launching the existing native software.

The strongest marketing opportunities are:

- Moving files between computers and phones without a network.
- Sending from a large screen to multiple receivers.
- Using encrypted pairing and groups.
- Demonstrating a real native optical protocol across operating systems.
- Explaining the engineering behind the transfer technology.

**Priority order:**

1. Validate the existing native cross-platform suite.
2. Package it for easy installation.
3. Build the public website.
4. Record compelling real demonstrations.
5. Begin targeted beta marketing.
6. Complete browser interoperability.
7. Pursue broader technology press coverage.
8. Validate monetization and commercial licensing.

**Success means making DataLux easy to discover, download, understand, test, and trust—while preserving the native engineering work already completed.**
