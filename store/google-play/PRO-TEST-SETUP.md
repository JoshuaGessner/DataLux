# Isolated Pro purchase test setup

Status: staging infrastructure and billing configured November 4, 2026; one test purchase,
on-device Pro activation, two-file batches and a refund/revocation check passed. Fresh-device
restore and additional batch acceptance remain outstanding. The Play internal track offers the staging-connected
0.4.11/code 15 build. Production billing is separately configured with test receipts disabled.
See [upload readiness](UPLOAD-READINESS.md)
for the current Play candidate and [test cases](TESTING.md) for expected results.

The NUC staging service uses `https://staging.datalux.anomalousinteractive.com`, loopback port
`8788`, Docker project `datalux-staging`, network `datalux-staging-backend` (`172.30.93.0/24`,
gateway `172.30.93.1`) and separate `datalux-staging-data` volume. Health and readiness passed
locally. Its test signer is private under the NUC's DataLux staging secrets directory; its public
SPKI SHA-256 begins `d1ed41f71b4b4156`, distinct from the production signer. It mounts the existing
SMTP configuration read-only. Staging alone allows only test receipts and has
the test signer and Publisher credentials mounted read-only. The non-secret compose environment
is at `~/.local/share/datalux/staging/compose.env`; it contains private file paths, not key bytes.

Production and staging run the digest-pinned web release `v0.3.5-preview.9` from revision
`87f1d888f0da505a3a7f755c20ab199190731c18`; both readiness endpoints passed after deployment.
Before the staging update, its isolated schema-5 database and matching key were backed up,
checked and restored to a temporary preflight volume; production used its deployment controller's
verified account backup. The staging service retains its separate origin and test-only purchase
settings, while production continues to reject test receipts. This does not prevent Google from charging
a real payment method; confirm the test notice and test payment method before any purchase.
The debug-signed staging APK is stored at
`build/artifacts/staging-pro-test/datalux-staging-debug.apk` (SHA-256
`1816550edec41f8c65846c901d5873278d8b0fd16cd3a2f5358561cf204ad82b`), built from the local
working tree with staging origin and test public key. It was installed in place over the same-signed
debug app on the owner's connected Android phone.
The Google Cloud project `datalux-play-billing` has the Google Play Android Developer API enabled.
The `datalux-play-purchase-verifier` service identity has DataLux-scoped Play billing permissions;
its private key was created on the NUC, and only its public certificate was registered with Google.
OAuth authentication succeeded, an invalid-token Publisher request returned HTTP 404, and a later
test purchase was verified. Its Google public key expires November 4, 2027.
The owner Google account is on a saved license-tester list. The `permanent` purchase option is
active; code 5 was active on the internal test track for the owner list at the time, unreviewed. Its Play build
uses production origin/trust and must not replace the staging APK during the staging purchase test.

On November 4, the owner verified a staging account and opted into the internal track. Google Play
then displayed “The item you were attempting to purchase could not be found.” No purchase or Pro
grant was confirmed at that time. The phone has only the owner Google account. The US-only buy option is active;
the production track's country controls were inaccessible because production access is not yet
granted. Retry after propagation with freshly queried product details before treating this as a
confirmed configuration or client defect. See [Google's billing error guidance](https://developer.android.com/google/play/billing/errors).

Later the same evening, the debug staging build queried the product and launched the Play billing
flow successfully. The owner reported completing a test purchase. The billing callback reported OK
with one purchase, and the isolated test-only backend recorded one granted, acknowledged purchase.
The owner confirmed the phone displayed “Pro is active.” The owner then signed out on that phone,
signed back into the same staging account and saw Pro active automatically. This confirms account
recovery on the same phone; a fresh-device restore still needs testing.
The exact reason the earlier checkout failed is not established; propagation is plausible.

The owner subsequently installed code 7 from Google Play over a fresh installation and signed in to
the same staging account; Pro appeared again without another purchase. This verifies account-based
recovery on the same phone, not a fresh-device restore or a separate Restore purchases flow.
Google Play then refunded the test order with entitlement removal. The staging purchase remained
granted until its scheduled check was advanced manually; the normal six-hour recheck then verified
the purchase with Google and marked it revoked. The owner confirmed the app showed Free after
refreshing. This tests the reconciliation path, not staging push delivery, notification retries,
or the complete PAY-05 matrix. Staging does not receive the production notification subscription.
Afterward the owner purchased Pro again in the Play-installed code 7 app and reported Pro active.
The test-only staging backend recorded a new granted, acknowledged purchase, while the refunded
purchase remained revoked.

The owner then sent `one.txt` and `two.txt` together from desktop to the Pro staging phone. Both
appeared on the phone. Their phone SHA-256 hashes matched the original test fixtures exactly
(`a4daf1949fc1df68…` and `7c07e7c8d1811b8a…` respectively). This proves one two-file batch
reached this device intact; the encryption mode was not recorded, and other batch cases remain open.
After updating the staging APK to use Downloads/DataLux, the owner confirmed the older files
remained visible and sent the same batch again. Both newly received files were in Downloads/DataLux,
and their phone SHA-256 hashes matched the originals. The legacy files remain in Downloads/FileFlow.

On November 4, the owner also paired the desktop with the staging phone, enabled encrypted mode,
and sent `encrypted-one.txt` and `encrypted-two.txt` together. Both arrived and the phone's file
hashes matched the originals above. This is one encrypted desktop-to-Pro-phone batch, not a broad
camera or device compatibility result. An initial pairing attempt stalled on desktop after Android
closed its confirmation quickly; a later attempt succeeded. With updated builds, the owner confirmed
that the first device could finish while the other was still waiting, and that scanning the first
device's updated code again completed pairing. Clearer instructions and confirmation controls are
being prepared; verify them on devices before treating the revised flow as accepted.

On November 6, a local desktop build with bright padding in the final partial data symbol sent a
67-byte fixture to the Play-installed 0.4.7 staging phone. The owner reported success at Steady,
Fast and Fastest; all three saved copies matched the source SHA-256. Fast's four-colour palette
represents the all-one padding as yellow; the other tested profiles display it as white. This
validates desktop-to-phone for this fixture and device only. Phone-to-desktop, larger files,
different lighting and the Play-distributed version of the changed sender remain to be tested.

Later the same day, the owner sent the 67-byte fixture from the Play-installed 0.4.9 staging
phone to the local desktop receiver with the experimental 84×182 Webcam profile and Low screen
brightness. The receiver saved a byte-identical file after an initially difficult alignment.
This is one successful transfer, not evidence of reliable acquisition across repeats or cameras.
The owner also confirmed that a local experimental desktop Fast sender using white padding sent
the same file to the unchanged 0.4.9 phone; the saved file hash matched. Phone-to-desktop with the
new Fast palette and degraded-pilot interoperability remain to be verified on physical devices.

The owner also encountered a misleading unverified screen after a restart while the phone could
not resolve the staging hostname. Server-side aggregate checks showed the account remained verified;
switching to cellular restored verified status. On the updated staging APK, the owner confirmed
the account displayed “Email verified” online, “Account status unavailable” in Airplane mode,
and recovered the verified display when connectivity returned. This validates that specific
physical-device scenario; it does not establish purchase, restore or all account edge cases.

## Isolation and build identity

- The owner-reviewed [0.4.5-staging/code 9 build](https://github.com/JoshuaGessner/DataLux/actions/runs/42147831164)
  was published to the existing internal track with settings and native autofill changes. Its source
  revision is `f42ab2ea8126591dcc9b407e8ea897bd49bb105b` and bundle SHA-256 is
  `28e05d7783f7c5660b34b6d55f133b32b2953d6ddde4f25616fc24a7fbaf9b4e`. Google Play reports it
  available to internal testers; physical settings and password-manager acceptance is pending.

- On November 5, the `datalux_pro` permanent buy option was activated in Play, US-only at $9.99.
  After the new purchase and sign-in recovery passed, the old `fileflow_pro` purchase option was
  deactivated for new sales; its product and historical transaction records remain. Staging was backed up, restored to a separate
  volume, and migrated to schema 5 from web revision `556be5c0f05ec71b13d02f2ce756f52e7bcecda4`;
  the old client retained Pro after migration. The owner refunded the remaining active legacy test
  order; the staging server independently rechecked and marked both legacy transactions revoked.
  Production was then backed up and upgraded to schema 5 using web release `v0.3.5-preview.7`;
  both production and staging reported healthy, and production retained test-receipt rejection.
- The signed internal staging bundle [run 42144852409](https://github.com/JoshuaGessner/DataLux/actions/runs/42144852409)
  used native revision `05f6dfed569bef4c93f6ed84b49e2586c322f610`, code 8,
  `0.4.4-staging`, and SHA-256 `1d5d3013d0d0746546e39396c7104c3a700c34567687c29a55a042f0770d7adc`.
  Google Play listed it as available to the existing internal track. The owner reported updating
  the existing installation and completing the Pro purchase. Staging recorded one granted,
  acknowledged `datalux_pro` transaction and two revoked `fileflow_pro` transactions. The owner
  confirmed the $9.99 test checkout, restart and automatic Pro recovery after signing out and in.
  Fresh-device restore and other device acceptance cases remain open.

- The owner selected a staging-connected Play test build for Pro testing. The dedicated
  `prepare Play STAGING test bundle` workflow uses the same Play upload identity but a separate
  staging license public key and a fixed staging origin. Its app label is **DataLux Test** and
  its version name ends in `-staging`. It produces an artifact for review; it does not upload or
  publish. Never promote a staging bundle to production. A later production build must use a higher
  version code, the production origin and the production license key. The internal track now uses
  the staging build; the previous 0.4.2 internal build used production.
- Owner-reviewed staging build [run 41969264446](https://github.com/JoshuaGessner/DataLux/actions/runs/41969264446)
  succeeded from `592335176772669e6140dc65cd95bf6e191101d6`. The signed bundle is version code 7,
  `0.4.3-staging`, label `DataLux Test`, with SHA-256
  `4ce35669eb8d0c0cd5202b04ed7ba2edaa2ca0e4f8dd6d6de5b10a6eca8cf329`. Its report and downloaded
  file agree on the staging origin, test key ID and established upload certificate. On November 4,
  Google Play showed it as available to internal testers. The owner installed code 7 from Google
  Play, recovered the existing staging Pro account, and completed a new test purchase after refund.
- Play-distributed and debug APKs have different signing identities. Switching may require
  uninstalling the debug app, which removes app-private state; preserve anything needed first.
  Testers must join the track and separately be enrolled as Google license testers, and must see
  Google's test-payment notice before confirming checkout. Use unique staging credentials, not a
  production password. Staging and production accounts and grants are independent.
- Verify DNS resolution from each test network for the dedicated HTTPS staging origin. The distinct
  Docker project, network, data volume and account database are established. Do not clone production accounts or purchase records into staging. Make the
  staging service and Caddy route identifiable and keep the production route and volume intact.
- Generate a separate P-256 test signer and retain its private key only on the staging server.
  Record the public key ID and origin; configure the staging service to allow test purchases.
  Keep the production signer and production `FILEFLOW_PLAY_ALLOW_TEST_PURCHASES=false` unchanged.
- Configure the Android Publisher API and a least-privilege Play service account able to verify
  and acknowledge this app's purchases. Mount its private credential file read-only, outside Git,
  images and artifacts. Verify quotas and package `dev.datalux`; retain historical `fileflow_pro`
  verification while the new `datalux_pro` product is prepared and tested.
- Build a test-only Android APK with `-Pstore=true`, `-PfileflowAccountOrigin=<staging HTTPS origin>`
  and `-PfileflowLicensePublicKey=<base64 staging public SPKI>`. Record source SHA, APK hash,
  app ID, origin, public key ID and signing certificate without exposing credentials. The debug
  APK and Play-signed app have different signatures; switching between them may require uninstalling
  and erasing app-local files. Preserve anything needed first.
- Google permits enrolled license testers to use a sideloaded debug-signed build with the same
  package name. Verify its exact Play account, test payment method and test-purchase notice. The
  staging APK cannot validate production-signed grants, and its staging accounts and grants must
  not be represented as production access. Validate a Play-distributed production build separately.

## Console and live rehearsal

1. Add consenting participants' Google Play accounts to the account-level license testers list;
   test-track membership is separate. The buy option is active. An active option and a published app
   release are separate controls; confirm the actual Console state before distributing anything.
2. Verify a disposable staging DataLux account and use Google's approved test payment method.
   Stop if the sheet does not explicitly show a test purchase. Do not ask testers to use real cards.
3. Run the [acceptance matrix](TESTING.md): purchase and acknowledgement, pending/declined,
   duplicate/retry, wrong account, restore on another device, offline grant, refund/revocation,
   and batch/bulk use. Record observed results and timestamps without receipts or private keys.
4. Rehearse legitimate Pro access for app review, notification authentication, production receipt
   rejection of test purchases, and recovery backups. Do not claim these checks passed until run.

References: [Google Play Billing testing](https://developer.android.com/google/play/billing/test)
and [license tester setup](https://support.google.com/googleplay/android-developer/answer/6062777).
