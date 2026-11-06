# DataLux Play upload readiness

> **Status:** Internal release and Pro purchase option active; billing acceptance in progress
> **Owner:** Release engineering
> **Last reviewed:** 2026-11-04
> **Related:** [Product policy](../../PRODUCT.md), [US listing materials](en-US/README.md), [Data Safety draft](DATA-SAFETY.md)

The owner chose **DataLux** and resumed Play preparation on October 29, 2026. The Android
application ID is now **`dev.datalux`**, confirmed by the owner after creating the Play entry.
The active product is `datalux_pro`; the legacy `fileflow_pro` purchase option is inactive for
new sales while historical transactions remain supported. Kotlin/JNI remains `dev.fileflow`. See
[the migration plan](../../DATALUX_MIGRATION.md): the new package is a separate Android
installation and does not inherit private state from historical `dev.fileflow` builds.

## Current evidence

The [0.4.9-staging/code 13 build](https://github.com/JoshuaGessner/DataLux/actions/runs/42314171042)
is available on the internal track. It was built from `7160d051a02b71e7de023c7689eded7b2eac59cc`;
its AAB SHA-256 is `8bb94e201073f1b2bedde0ef9c5bbb679719c6572a75776b13fb569d1b7fa51c`.
It includes brighter small-file padding and the experimental Webcam sending profile. The padding
change passed a desktop-to-phone physical test on all three desktop speeds, but the Play-installed
build and the phone-to-desktop path still require physical acceptance.

The earlier [0.4.6-staging/code 10 build](https://github.com/JoshuaGessner/DataLux/actions/runs/42151916912)
was previously available on the internal track. It was built from `ee92163e07a2ca67a2ade8d936f08bff68815570`;
its AAB SHA-256 is `99449718b6d5909fffe8ec268cbf4a3bf76ba3e98139f214bb30d20b11c3979b`.
It added the Pro sign-in layout and ordinary pairing confirmation changes. The owner confirmed
the new sign-in layout and Google Password Manager worked. Team invitations still exchange the team key before
human code confirmation and need a coordinated security fix before broader testing or launch.

The earlier 0.4.5-staging/code 9 bundle added settings and native autofill improvements. The owner
confirmed Google Password Manager works after choosing it as the phone's autofill provider.

On November 5, the staging-connected 0.4.4/code 8 bundle replaced code 7 on the existing internal
track. It targets the new `datalux_pro` option and the schema-5 staging service. The owner reported
updating and completing a $9.99 test purchase; staging recorded it as granted and acknowledged.
The owner confirmed Pro remains active after restarting and returns after sign-out/sign-in.

On November 4, the owner-approved [0.4.2/code 6 build](https://github.com/JoshuaGessner/DataLux/actions/runs/41963741276) succeeded from `fa0c1f7`. The downloaded bundle SHA-256 was `54fb5cdf924b746f090c7a39ce0a3fdaa74afde1a1b52e97b955cf8b861bdf74`, matching its verification report. Google Play shows this version available on the internal tester track. It uses the production origin and signer; production billing is configured with test purchases disabled, and staging accounts and grants are separate. Real production purchase/restore acceptance is not established. The code 5 entries below are historical.

Later on November 4, the owner approved the staging-connected [0.4.3/code 7 bundle](PRO-TEST-SETUP.md),
which Google Play shows available on the internal tester track. The preceding code 6 track status is
historical. This is a test-only build; never promote its staging trust or accounts to production.

The table below preserves an earlier audit; consult the current evidence above and the Console for
the latest build and test status.

| Item | Recorded state |
| --- | --- |
| Developer account | Verified Personal account; owner reports the DataLux app entry is created |
| Console app | Independently checked: DataLux / `dev.datalux`, draft; dashboard app ID `4975860505031813621`. Privacy URL, no-ads, non-government, no-financial-features, no-health-features and no-Advertising-ID answers saved as pending review changes; Tools category selected |
| Current CI | Native main `914f175` passed CI including sanitizers and desktop CI. Its owner-dispatched [0.4.1 review build](https://github.com/JoshuaGessner/DataLux/actions/runs/41881278200) passed Android, Linux, Windows and macOS jobs. Web main `518b372` passed CI and the [web review-image build](https://github.com/JoshuaGessner/DataLux-Web/actions/runs/41882637955). These candidates were not published or deployed |
| Candidate version | 0.4.1 / Android version code 5; recheck unused Play version code before upload |
| Earlier local checks | October 29 Android debug APK, unit tests, debug/release lint, release manifest with `dev.datalux` and existing JNI classes, 16 release-helper tests and relative documentation links passed. Backend tests cover DataLux Publisher URLs and rejection of old-package notifications. These checks do not validate the final signed artifact |
| Signed Play bundle | Owner approved [0.4.1/code 5 review run](https://github.com/JoshuaGessner/DataLux/actions/runs/41951982131), which succeeded on November 4 from `914f175`. Downloaded AAB SHA-256 `77817a40269c7d40fa05a65faaac14eb6f7f328e8752c654aa50f5ecbd0f746c` matches the report; package, upload certificate and license key ID also match. The internal track is active for the owner list, with code 5 (0.4.1), arm64, API 33+, not reviewed. This production-origin build predates the staging build and is not a staging APK |
| Play upload identity | Permanent RSA-3072 encrypted upload keystore created in private local storage; re-open/sign/verify challenge passed. Four upload secrets configured in owner-reviewed, main-only `play-internal` environment. Off-device recovery backup not yet verified |
| License trust | On November 4, production config had no billing signer and the purchase table was empty. A new P-256 signer was created privately on the NUC; matching public/private key ID `4b72019e9420f53f` was verified and its public key set in `play-internal`. The private key remains on the NUC. An encrypted off-device recovery backup remains outstanding; do not rotate or lose this key after distributing builds |
| Server access | NUC was inspected November 4; live service was healthy on `v0.3.5-preview.5`. A new verified account snapshot was restored to a separate temporary volume and checked at schema 4. The temporary volume was removed. The snapshot does not include the separate license signer; off-device recovery and retention remain outstanding |
| Play-managed signing | Console shows the app-signing key in use. It is separate from the locally created upload key |
| Payments and testers | The app is Free. Google Play shows `fileflow_pro` / `permanent` active, United States only, at $9.99. The internal track is active for the owner list; opt-in and purchase tests remain to verify. The closed-test dashboard showed 0 testers. The owner's Google account is configured as a license tester |
| Content rating | IARC completed October 29; North America ESRB Teen, Users Interact and In-App Purchases. [Answer record](CONTENT-RATING.md) explains the actual file-sharing behavior; Google app review/release remain pending |
| Remaining Console declarations | Sign-in/reviewer access, target audience and Data Safety remain incomplete. Dashboard shows 8 of 11 setup tasks complete. Audience is blocked until legitimate reviewer access exists; do not submit placeholder credentials. Nothing sent for Google app review |
| Artwork and listing | US text, icon, feature graphic and four genuine 1080 × 1920 phone screenshots saved; Console says Ready to send for review. AI labels applied to prepared icon/feature artwork, not direct screenshots. No Google app-review submission or publication |
| Connected phone | Debug `dev.datalux` 0.4.1/code 5 launches; Send/Receive home, Settings, Pro, Received and Teams visually checked on SM-S948U1 / Android 17. Display restored after captures; no camera permission, account, purchase or file changes. Physical phone page size 4096; separate 16 KB emulator evidence is recorded below |
| Native compatibility | [Compatibility record](ANDROID-COMPATIBILITY.md): debug ELF/RELRO and 16 KB ZIP alignment passed. Android 17 ARM64 emulator reports 16384-byte pages; native smoke passed 37 compression round trips, 96 frames, buffer/hash/receiver checks and cold launch with fallback disabled. Final signed-build and physical optical acceptance remain pending |
| Test preparation | [Tester guide and acceptance matrix](TESTING.md) prepared; planned transfer/billing cases remain NOT RUN. [Screenshot provenance](en-US/screenshots/README.md) records the actual debug build and device checks |

## Product setup after the first upload

The payments-profile step is complete. The signed billing-enabled bundle is uploaded to an
internal track and the purchase option below is active. Recheck the live Console state and finish
the isolated test setup before asking anyone to make a test purchase:

- Product ID: `datalux_pro` (active replacement); `fileflow_pro` remains as an inactive legacy
  product for historical purchase verification. Older builds cannot start a new legacy purchase.
- Default title: **DataLux Pro**.
- Description: **Unlock multi-file batch transfers and bulk actions on received files.**
- Exactly one permanent buy option, non-consumable, quantity one; no rental or subscription.
- US availability only, **$9.99 USD** base price. Verify the actual Google checkout sheet later.

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
7. Configure Publisher credentials and authenticated purchase
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
