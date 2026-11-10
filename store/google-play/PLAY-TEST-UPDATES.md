# Play test release and update runbook

> **Status:** Coordinator runbook. Check [upload readiness](UPLOAD-READINESS.md), the staging
> evidence and live Console for current status; a successful signing run alone is not a release.

At the last confirmed Console check, 0.4.9-staging / code 13 was available to the existing internal
track. The reviewed 0.4.10-staging / code 14 bundle built successfully but was not confirmed as
uploaded or published. Confirm the Console's current track, highest uploaded code, and bundle
before acting. Its staging identity is separate from production; never promote that bundle to a
production track. The [tester quick start](TESTER-QUICK-START.md) still needs a feedback channel
and a confirmed companion build before distribution.

## Before the first internal release

1. Review the source and the intended test scope. Start with the small internal group; the closed
   track has separate setup and eligibility requirements. Verify account, email and deletion
   behavior and the appropriate signing/backup prerequisites in [upload readiness](UPLOAD-READINESS.md).
2. On the NUC, inspect the existing production license signer and verify its public key and backup.
   Use the existing signer if present; do not replace it merely to build. Configure the verified
   public key and account origin in the protected GitHub `play-internal` environment. The Android
   upload keystore, Google's Play app-signing key and the backend license signer are distinct.
3. Check Play Console for previously uploaded version codes and select an unused, greater code.
   Confirm package `dev.datalux`. The active product at the last check was `datalux_pro`;
   historical `fileflow_pro` purchases remain supported.
4. Complete relevant Console configuration, including a real, reusable reviewer account with
   access to restricted features; review Data Safety against the actual deployment. Do not submit
   placeholder credentials or unverified privacy assertions.

## Build and review each candidate

1. Merge the intended fixes into `main`, wait for required checks, and record the **full immutable
   source SHA**. Make sure the SHA still contains the fixes to be tested.
2. In GitHub Actions, select **prepare Play internal-test bundle**, then **Run workflow** from
   `main`. For staging, select **prepare Play STAGING test bundle** and use a version ending in
   `-staging`. Enter the full SHA as `source_ref` and an unused `version_code`. Only the repository
   owner can dispatch it; GitHub's `play-internal` environment requires owner review.
3. Wait for the entire run. Download its `datalux-STAGING-play-review-<version_code>` artifact for
   staging (or `datalux-play-review-<version_code>` for production) and inspect
   the AAB and `PLAY-BUNDLE-REVIEW.json`: source SHA, bundle hash, package, version and code,
   DataLux label, account origin, upload certificate, license public-key ID, and native 16 KB
   alignment. Compare signing identities with the intended existing identities. The workflow
   runs automated checks; it **does not upload or release the bundle**.
4. Keep the report, source SHA and artifact hash with the release record. Actions artifacts have
   limited retention; archive what is needed privately. Never archive credentials, passwords,
   private keys or purchase tokens in the report or Git.

## Deliver to a testing track

1. In Play Console select DataLux → **Test and release → Testing → Internal testing** (or the
   intended closed track), then create a release. Upload the reviewed AAB, add accurate release
   notes and resolve any errors. Review package, version, target track and country availability.
2. Follow Console's steps to review and roll out the release, including any Google review or
   publishing step it requests. Uploading a bundle or saving a draft alone does not make it
   available to testers. Confirm the resulting track status and version in Console.
3. Add the intended Google accounts or group to the track, set a private feedback channel, and
   share the track's actual opt-in link when available. Testers must opt in and install from Play.
   Do not substitute the experimental GitHub debug APK for the signed Play-distributed build.
4. Record opt-ins, tester feedback and exact build/device results. A testing-track member is not
   automatically a billing license tester: configure license testers separately, use the isolated
   approved billing test setup, and require Google's test-purchase notice and a test payment method
   before a test purchase. Stop if the expected notice, payment method or account is missing.

Do not ask outside testers to use team invitations until bilateral confirmation is fixed and
verified. Phone-to-desktop acquisition, including the 0.4.10 white Fast sender, still needs
repeatable real-device acceptance; one successful Webcam-mode transfer does not establish it.

## Fix and repeat

Triage feedback, fix and test the issue, then repeat the build and review steps with the updated
commit and a **new version code greater than every code already uploaded**. Roll out the new AAB
to the intended track and check Console for its actual availability. Testers normally receive
updates via Google Play; they can also check the app listing for an update. Ask them to report
their installed version, since availability and device update timing vary. Test-track membership
is maintained by the track configuration; do not remove testers while tracking their continuous
closed-test eligibility. Record the Console's eligibility date instead of assuming it.

Before moving to a closed test, complete Console app setup, fix release blockers found internally,
provide a private feedback route and a tested opt-in link, and rehearse the reviewer path. For this
qualifying Personal account, Google requires at least 12 testers continuously opted in for the
preceding 14 days before applying for production access. Internal testing does not satisfy it.
Moving an internal tester to closed testing may require opting out of internal first; confirm the
track and installed build, and allow for propagation. Google can require further testing or review.

References: [create and update releases](https://support.google.com/googleplay/android-developer/answer/9859348),
[test tracks](https://support.google.com/googleplay/android-developer/answer/9845334),
[billing tests](https://developer.android.com/google/play/billing/test), and
[production access requirements](https://support.google.com/googleplay/android-developer/answer/14151465).
