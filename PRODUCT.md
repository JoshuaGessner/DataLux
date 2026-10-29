# DataLux Free and Pro

> **Status:** Accepted product policy; Play production acceptance pending
> **Owner:** Product / Android shell
> **Last reviewed:** 2026-10-28
> **Related:** ADR-0016, APP-01, APP-02, NATIVE_RELEASES.md

Owner-approved launch policy, October 28, 2026. This policy supersedes older proposals for paid
desktop features. Google Play production approval and public sales are still pending.

## Distribution and price

- **Desktop:** every feature is free on macOS, Windows and Linux. No account or activation is
  required for transfers, batch workflows, teams, multiple displays or presenter controls.
- **Android Free:** unlimited one-file-at-a-time sending and receiving at the available link speed.
  Compression, encryption, pairing, teams and individual file open/share/resend/delete remain free.
- **Android Pro:** one non-consumable Google Play purchase, product `fileflow_pro`, with a **$10 USD
  US launch price**. It adds multi-file batch sending, multi-file batch receiving, and bulk actions
  on received files. Google Play's localized product price and checkout are authoritative.
- **Ownership:** Pro belongs to a verified DataLux account. Sign into that account on another phone
  and restore/refresh ownership; it is not a separate charge for each phone. Verified activation is
  cached for offline use. Accounts and network access are unnecessary for Free transfers.
- **Launch territory:** United States only for the Play application and Pro buy option. Expand
  deliberately after reviewing product availability, local pricing, support and country requirements.
  Do not restrict already owned offline features using an IP/geolocation check.
- **Source:** the AGPL core stays open. Normal Android builds enforce Free/Pro; `-Pstore=false`
  explicitly creates a fully unlocked community build. Desktop always remains unlocked, even with
  an obsolete `FILEFLOW_STORE_BUILD=ON` setting. Historical community binaries are not retroactively
  changed or represented as paywalled releases.

## Feature matrix

| Capability | Android Free | Android Pro | Desktop |
| --- | --- | --- | --- |
| Send/receive one file at a time | Yes | Yes | Yes |
| All available speed profiles; automatic/maximum compression | Yes | Yes | Yes |
| Encrypted pairing and teams | Yes | Yes | Yes |
| Open/share/resend/delete an individual received file | Yes | Yes | Yes |
| Send multiple files as one batch | No | Yes | Yes |
| Receive and save multiple files from one batch | No | Yes | Yes |
| Select/share/resend/delete multiple received files together | No | Yes | Yes |
| Multiple sending displays and presenter controls | Not applicable | Not applicable | Yes |

The paid value is repeat-work convenience: field documents, photo/document sets, classroom handouts,
and other repeated offline workflows. Security, transfer integrity and ordinary interoperability are
not upsells. There are no artificial speed throttles or daily transfer counters. Existing technical
file-size, memory and hardware limits still apply; never advertise unlimited capacity or universal
camera compatibility. Folder recursion, background sync, cloud storage and scheduling are not sold
as existing features.

## Receiving-phone upgrade behavior

The receiving Android phone enforces its own account's Pro access. A free desktop or licensed sending
phone does not grant a Free receiver a license. Send one file at a time to interoperate for free.

Current senders carry an optional, anonymous batch advisory in the manifest. The app can show one
upgrade/restore prompt while scanning. This hint cannot grant access and does not prevent continued
scanning: a faulty or hostile hint must not block a legitimate single file. Old senders remain
compatible and may be identified as batches only after the transfer completes.

The final check uses the actual verified envelope, after encryption authentication and bounded
decompression, before allocating batch members or saving files. A legacy one-member batch remains
Free. A blocked multi-file batch saves nothing and offers Pro/restore or a single-file alternative.
If the completed batch's receive screen remains alive, the owner can return after activation and
choose **Save batch after upgrade** without retransmitting. Closing that screen or losing the
process requires receiving again. Early checkout stops the receive screen/camera and requires a
new scan. No purchase is started automatically, and previously saved files remain accessible.

## Purchase and refund limits

Only verified Google purchases or a cryptographically verified account-bound activation unlock Pro.
Pending/cancelled purchases do not. Restore, duplicate delivery, account mismatch, failed
acknowledgement, refunds and authenticated notifications are release acceptance tests.

Current activation files are perpetual and have no periodic expiry. A device without connectivity
cannot reliably learn about a refund or account deletion, and copied offline activation files are
not unbreakable DRM. Connected purchase reconciliation must still stop issuing new revoked grants.
Do not promise immediate remote revocation of offline installations or delete files to enforce a
license change. Website checkout is not implemented and is not required for this Play launch.

## Google Play release gates

1. The personal developer account is verified. The owner explicitly deferred app creation and
   Play publication pending a new app name. The selected display name is DataLux; the existing
   Android application ID remains `dev.fileflow`. Confirm the registration details before any Play upload. A draft
   form was inspected and closed without submission or acceptance of any declaration. At creation,
   the owner must review policy declarations, signing terms and the US export/encryption certification.
2. Establish stable upload signing, Play App Signing and a backed-up production license-signing key.
   Configure the protected `play-internal` environment; never put private keys into Git or bundles.
3. Create `fileflow_pro` with exactly one permanent buy option, US availability and $10 USD pricing.
   Configure least-privilege Publisher credentials and authenticated real-time purchase notifications.
4. Test a signed store AAB on a separate staging service with test purchases. Production must reject
   test receipts. Keep the same stable production license public key in all production Android builds.
5. Finalize listing assets, reviewer access, Data Safety and account deletion/backup retention. An
   operator must choose and enforce the retention period; current wording is not a completed policy.
6. Run real optical/account/billing acceptance and the qualifying personal-account closed test:
   at least 12 continuously opted-in testers for 14 days, then apply for production access.
7. Submit the reviewed production build. Initial release reaches eligible users in chosen countries;
   percentage staged rollout is for updates. Keep payments closed until the real acceptance gates pass.

See [NATIVE_RELEASES.md](NATIVE_RELEASES.md) and the separate website repository's
[Play checklist](https://github.com/JoshuaGessner/DataLux-Web/blob/main/PLAY_LAUNCH.md).

## Research behind the choices

Free network-sharing alternatives already cover general nearby transfer; DataLux's differentiated
workflow is screen-to-camera transfer without Wi-Fi, Bluetooth or a network. Keeping individual
transfers and security free lets people evaluate that difference, while the one-time convenience
upgrade serves repeat users. This is product judgment, not a claim of validated conversion rates.

- [LocalSend's free, network-based model](https://localsend.org/)
- [Google Play country/region targeting](https://support.google.com/googleplay/android-developer/answer/7550024)
- [Google Play one-time product setup](https://developer.android.com/google/play/billing/one-time-products)
- [Personal-account testing](https://support.google.com/googleplay/android-developer/answer/14151465)
- [Production rollout](https://support.google.com/googleplay/android-developer/answer/9859348)
