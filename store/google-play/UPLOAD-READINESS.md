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
| Console app | Independently checked: DataLux / `dev.datalux`, draft; dashboard app ID `4975860505031813621`. Privacy URL, no-ads, non-government, no-financial-features, no-health-features and no-Advertising-ID answers saved as pending review changes; Tools category selected |
| Current CI | Native main `edccb3c` passed all CI, including sanitizers; desktop CI and all four review packages passed. Web main `3e8374b` CI passed |
| Candidate version | 0.4.1 / Android version code 5; recheck unused Play version code before upload |
| Local candidate checks | Android debug APK, unit tests, debug/release lint, release manifest with `dev.datalux` and existing JNI classes, 16 release-helper tests and relative documentation links passed. Backend tests cover DataLux Publisher URLs and rejection of old-package notifications |
| Signed Play bundle | Not built: the production license public key is still required; release guard correctly refuses missing trust. Upload signing is now configured |
| Play upload identity | Permanent RSA-3072 encrypted upload keystore created in private local storage; re-open/sign/verify challenge passed. Four upload secrets configured in owner-reviewed, main-only `play-internal` environment. Off-device recovery backup not yet verified |
| License trust | Production signer's public key and backup not yet verified; never substitute an ephemeral test key |
| Server access | Owner is off the NUC network and explicitly deferred server work; [NUC handoff](NUC-HANDOFF.md) records all pending checks |
| Play-managed signing | Console shows the app-signing key in use. It is separate from the locally created upload key |
| Payments and testers | Owner completed the payments profile October 29; the merchant-creation gate has cleared. App pricing independently verified Free. One-time products now requires the first uploaded artifact with BILLING permission; the merged release manifest already includes it. No product or Play bundle uploaded; closed-test dashboard shows 0 testers |
| Content rating | IARC completed October 29; North America ESRB Teen, Users Interact and In-App Purchases. [Answer record](CONTENT-RATING.md) explains the actual file-sharing behavior; Google app review/release remain pending |
| Remaining Console declarations | Sign-in/reviewer access, target audience and Data Safety remain incomplete. Dashboard shows 8 of 11 setup tasks complete. Audience is blocked until legitimate reviewer access exists; do not submit placeholder credentials. Nothing sent for Google app review |
| Artwork and listing | US text, icon, feature graphic and four genuine 1080 × 1920 phone screenshots saved; Console says Ready to send for review. AI labels applied to prepared icon/feature artwork, not direct screenshots. No Google app-review submission or publication |
| Connected phone | Debug `dev.datalux` 0.4.1/code 5 launches; Send/Receive home, Settings, Pro, Received and Teams visually checked on SM-S948U1 / Android 17. Display restored after captures; no camera permission, account, purchase or file changes. Runtime page size 4096: 16 KB runtime still untested |
| Test preparation | [Tester guide and acceptance matrix](TESTING.md) prepared; planned transfer/billing cases remain NOT RUN. [Screenshot provenance](en-US/screenshots/README.md) records the actual debug build and device checks |

## Product setup after the first upload

The payments-profile step is complete, but Console still requires an uploaded billing-enabled
artifact before allowing one-time product creation. The existing merged release manifest declares
`com.android.vending.BILLING`; no permission fix or temporary debug upload is needed. After the
NUC signer is verified, prepare/review the real signed candidate, upload it to the internal track,
then create the product:

- Product ID: `fileflow_pro` (retained permanent identifier; do not rename it).
- Default title: **DataLux Pro**.
- Description: **Unlock multi-file batch transfers and bulk actions on received files.**
- Exactly one permanent buy option, non-consumable, quantity one; no rental or subscription.
- US availability only, **$10.00 USD** base price. Verify the actual Google checkout sheet later.

The Console also displayed a **15% service-fee enrollment** notice. Enrollment/eligibility is not
verified. Before sales, the owner should review associated-account declarations and the program
terms; do not accept declarations about other accounts without the owner's facts and approval.
See [Google's enrollment guidance](https://support.google.com/googleplay/android-developer/answer/10632485).

## Build and review sequence

1. App creation is reported complete by the owner. Confirm the Console package matches
   `dev.datalux` before upload and inspect any remaining registration/signing tasks. Creating
   the app is separate from distributing a release. Keep production closed.
2. The upload keystore and protected CI upload secrets are established. The private local identity
   is under `~/.config/datalux/signing/play-upload-20261029/`; never put its keystore/password file
   in Git or a public artifact. Verify a secure off-device recovery backup before release.
   Public certificate SHA-256: `68ece5cdea2e1698cee6073ff913663c9ddfcc6c9d2d5ad50a47468910724909`.
   Google-managed distribution signing and the backend license signer remain separate identities.
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
