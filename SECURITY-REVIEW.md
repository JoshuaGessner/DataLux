# Bug and security review — 2026-10-25

Reviewed the native transfer and envelope boundaries, Android share handling and
pairing lifetime, desktop receive/save paths, desktop cryptography and team-key
storage, and the pinned desktop dependencies. This is a focused source review
with regression testing, not a claim that every possible defect has been ruled out.

## Fixes

- Reject transfer manifests containing whole padding-only blocks. A one-byte file
  could previously request gigabytes of decoder allocation. Cap the estimated
  decoder working set at 512 MiB, including matrices and block overhead; unusual
  symbol geometry can reach this limit below the protocol's file-size maximum.
- Reject content hints that make a manifest exceed its frame's symbol buffer.
  Preserve the previous manifest on failure and propagate the error to the sender.
- Android rejects `file://` shares before reading them with its own privileges,
  preserves the saved encryption/team settings for share-sheet sends, and checks
  generated payload sizes before allocation.
- Discard expired pairing keys, including when the wall clock goes backwards,
  rather than allowing a later clock change to revive them.
- Desktop receives claim filenames atomically with exclusive creation. Existing
  files and dangling symlinks are never overwritten; close/write failures and
  partially saved batches no longer report full success. Drive prefixes, alternate
  data streams, and reserved Windows device names become ordinary filenames.
- Desktop cryptography checks entropy, key-generation, hashing and encryption
  failures, validates sealing inputs, and fails closed. UI and receive-worker
  exception boundaries report failure rather than silently continuing.
- Desktop team stores are written to a new private file (0600 on POSIX; an
  owner-only DACL on Windows), flushed, then atomically replaced. A failed save
  preserves the previous store and in-memory state. Symlinks are replaced rather
  than followed. Keys are still not wrapped in a hardware keychain.
- Native file-dialog callbacks retain an independent result mailbox so closing
  the app cannot leave the callback pointing to a destroyed App object.
- Update Mbed TLS to 3.6.7 and SDL to 3.4.18. See the upstream
  [Mbed TLS security release](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7)
  and [SDL release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18).
- Apply explicit downstream guards to hash-pinned quirc 1.2 for its
  [unbounded QR search](https://github.com/dlbeer/quirc/issues/162) and
  [null zero-length copy](https://github.com/dlbeer/quirc/pull/159).
  Search work is capped, impossible grid/area estimates are rejected, and the
  estimate arithmetic avoids signed integer multiplication overflow. The search
  cap may reject exceptionally distorted QR images; normal and mirrored pairing
  codes remain covered by tests. Configuration fails if a future version changes
  the patched source sites, requiring explicit review.

## Verification and builds

- Android: 48 JVM tests passed; debug lint has zero errors and six existing
  UI/performance/style warnings. Debug and release APKs build. Release signing is
  not configured; the connected phones use the debug-signed build, matching their
  existing installation and preserving app data.
- Emulator checks reject a private `file://` share and a 2 GiB generated-payload
  request with an error screen, without starting transmission.
- macOS release: the full native/desktop suite passes, with three pre-existing
  calibration/sweep tests disabled. The application signature verifies and the
  app passes a launch smoke test.
- Run the same suite under ASan/UBSan, including C dependency code, before delivery.
- `.github/workflows/desktop-builds.yml` builds Linux and Windows from `main`,
  tests Linux, smoke-launches both, and uploads revision-labelled artifacts.

Fresh builds do not constitute a physical optical transfer test. Real camera,
lighting, and phone-to-desktop pairing interoperability still need hands-on
validation. macOS uses ad-hoc signing, Windows is unsigned, and the Linux build
targets Ubuntu 24.04 or a compatible system.
