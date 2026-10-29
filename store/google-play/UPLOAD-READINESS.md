# DataLux Play upload readiness

> **Status:** In progress; no Play upload or production launch recorded
> **Owner:** Release engineering
> **Last reviewed:** 2026-10-29
> **Related:** [Product policy](../../PRODUCT.md), [US listing materials](en-US/README.md), [Data Safety draft](DATA-SAFETY.md)

The owner chose **DataLux** and resumed Play preparation on October 29, 2026. The Android
application ID is now **`dev.datalux`**, confirmed by the owner after creating the Play entry.
The permanent Pro product stays `fileflow_pro`; Kotlin/JNI remains `dev.fileflow`. See
[the migration plan](../../DATALUX_MIGRATION.md): the new package is a separate Android
installation and does not inherit private state from historical `dev.fileflow` builds.

## Current evidence

| Item | Recorded state |
| --- | --- |
| Developer account | Verified Personal account; owner reports the DataLux app entry is created |
| Console app | Owner-confirmed `dev.datalux`; dashboard app ID `4975860505031813621`. Browser access is disabled, so remaining Console settings have not been independently rechecked |
| Previous CI | Native main `9a5e16f` and web main `9943cf2` passed; earlier paywall CI also completed successfully |
| Candidate version | 0.4.1 / Android version code 5; recheck unused Play version code before upload |
| Local candidate checks | Android debug APK, unit tests, debug/release lint, release manifest with `dev.datalux` and existing JNI classes, 16 release-helper tests and relative documentation links passed. Backend tests cover DataLux Publisher URLs and rejection of old-package notifications |
| Signed Play bundle | Not built: the production license public key and permanent upload identity are required; release guard correctly refuses missing trust |
| Play upload identity | No `play-internal` environment/secrets configured when inspected; permanent upload keystore not yet verified |
| License trust | Production signer's public key and backup not yet verified; never substitute an ephemeral test key |
| Server access | Existing NUC SSH endpoint timed out; production/staging billing configuration remains unverified |
| Artwork and listing | US text, 512 × 512 icon and 1024 × 500 feature graphic prepared; genuine Android screenshots and reviewer rehearsal remain required |

## Build and review sequence

1. App creation is reported complete by the owner. Confirm the Console package matches
   `dev.datalux` before upload and inspect any remaining registration/signing tasks. Creating
   the app is separate from distributing a release. Keep production closed.
2. Reuse or securely establish a permanent, backed-up upload keystore outside the checkout.
   Keep it separate from Google's distribution signing key and the backend's license signer.
   Configure the protected GitHub `play-internal` environment using the settings in
   [the web checklist](https://github.com/JoshuaGessner/DataLux-Web/blob/main/PLAY_LAUNCH.md).
3. Verify the production license public key against the real signer and configure the final
   account origin. No private license key belongs in CI, Git, an AAB or an image.
4. Commit/push the reviewed source to main. Dispatch **prepare Play internal-test bundle**
   with its full commit SHA, version 0.4.1 and an unused version code (currently planned 5).
   The workflow produces a review artifact and never uploads automatically.
5. Check the AAB/APK signature, DataLux label, package, versions, target SDK, native and ZIP
   16 KB alignment, backup exclusions, HTTPS policy, trust-key ID and account origin. The
   review report records source/certificate/bundle hashes. Runtime 16 KB testing is separate.
6. Review the exact bundle and upload it to internal testing. Preserve device files before
   replacing debug builds, since signing identities may differ. Do not enable public sales.
7. Provision the one-time US $10 buy option, Publisher credentials and authenticated purchase
   notifications. Test with Google's license testers on a separate staging backend/database;
   production must reject test receipts. Use the same production trust for production releases.
8. Rehearse free transfers, paid receive/send/bulk, restoration, pending/cancelled/refunded
   purchases, account mismatch/deletion and offline behavior. Record actual devices/results.
9. Finish listing assets, privacy/retention, Data Safety, rating, reviewer access and closed-test
   setup. For the qualifying Personal account, run at least 12 opted-in testers continuously
   for 14 days before applying for production access. Internal testing does not start this clock.

No production readiness, real purchase success, physical camera acceptance or store approval is
implied by a successful source build. Record each outcome here when it is actually verified.

Primary sources: [app setup](https://support.google.com/googleplay/android-developer/answer/9859152),
[Personal-account testing](https://support.google.com/googleplay/android-developer/answer/14151465),
[target API](https://developer.android.com/google/play/requirements/target-sdk),
[16 KB runtime guidance](https://developer.android.com/guide/practices/page-sizes).
