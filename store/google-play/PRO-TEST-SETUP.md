# Isolated Pro purchase test setup

Status: staging infrastructure and billing configured November 4, 2026; public DNS propagation,
test purchase and paid-feature acceptance remain outstanding. See [upload readiness](UPLOAD-READINESS.md)
for the current Play candidate and [test cases](TESTING.md) for expected results.

The NUC staging service uses `https://staging.datalux.anomalousinteractive.com`, loopback port
`8788`, Docker project `datalux-staging`, network `datalux-staging-backend` (`172.30.93.0/24`,
gateway `172.30.93.1`) and separate `datalux-staging-data` volume. Health and readiness passed
locally. Its test signer is private under the NUC's DataLux staging secrets directory; its public
SPKI SHA-256 begins `d1ed41f71b4b4156`, distinct from the production signer. It mounts the existing
SMTP configuration read-only. Staging alone allows only test receipts and has
the test signer and Publisher credentials mounted read-only. The non-secret compose environment
is at `~/.local/share/datalux/staging/compose.env`; it contains private file paths, not key bytes.

The staging image is `datalux-staging:6bad4cd`, built on the NUC from web revision
`6bad4cd55abc824cd65340b4829605fac3e337b6`. Its health endpoint reported that revision and
schema 4, both locally and through Caddy with a valid TLS certificate. It includes the
acknowledgement retry fix and rejects non-test receipts. This does not prevent Google from charging
a real payment method; confirm the test notice and test payment method before any purchase.
The debug-signed staging APK is stored at
`build/artifacts/staging-pro-test/datalux-staging-debug.apk` (SHA-256
`fdfcaafbd10ed3e9d1650da21a411002c630c40c8d802a6c9235bea29a31dc38`), built from the local
working tree with staging origin and test public key. It was installed in place over the same-signed
debug app on the owner's connected Android phone.
The Google Cloud project `datalux-play-billing` has the Google Play Android Developer API enabled.
The `datalux-play-purchase-verifier` service identity has DataLux-scoped Play billing permissions;
its private key was created on the NUC, and only its public certificate was registered with Google.
OAuth authentication succeeded and an invalid-token Publisher request returned HTTP 404; an actual
purchase is still needed to prove the full flow. Its Google public key expires November 4, 2027.
The owner Google account is on a saved license-tester list. The `permanent` purchase option is
active; code 5 is active on the internal test track for the owner list, unreviewed. Its Play build
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
The exact reason the earlier checkout failed is not established; test-track/product propagation is
plausible. Confirmation of the on-device Pro screen, batch operation, restore and refund remain separate.

The owner also encountered a misleading unverified screen after a restart while the phone could
not resolve the staging hostname. Server-side aggregate checks showed the account remained verified;
switching to cellular restored verified status. On the updated staging APK, the owner confirmed
the account displayed “Email verified” online, “Account status unavailable” in Airplane mode,
and recovered the verified display when connectivity returned. This validates that specific
physical-device scenario; it does not establish purchase, restore or all account edge cases.

## Isolation and build identity

- Complete the dedicated HTTPS staging origin; the distinct Docker project, network, data volume and
  account database. Do not clone production accounts or purchase records into staging. Make the
  staging service and Caddy route identifiable and keep the production route and volume intact.
- Generate a separate P-256 test signer and retain its private key only on the staging server.
  Record the public key ID and origin; configure the staging service to allow test purchases.
  Keep the production signer and production `FILEFLOW_PLAY_ALLOW_TEST_PURCHASES=false` unchanged.
- Configure the Android Publisher API and a least-privilege Play service account able to verify
  and acknowledge this app's purchases. Mount its private credential file read-only, outside Git,
  images and artifacts. Verify quotas and the deployed package `dev.datalux` and SKU `fileflow_pro`.
- Build a test-only Android APK with `-Pstore=true`, `-PfileflowAccountOrigin=<staging HTTPS origin>`
  and `-PfileflowLicensePublicKey=<base64 staging public SPKI>`. Record source SHA, APK hash,
  app ID, origin, public key ID and signing certificate without exposing credentials. The debug
  APK and Play-signed app have different signatures; switching between them may require uninstalling
  and erasing app-local files. Preserve anything needed first.
- Google permits enrolled license testers to use a sideloaded debug-signed build with the same
  package name. Verify its exact Play account, test payment method and test-purchase notice. The
  staging APK cannot validate production-signed grants, and its staging accounts and grants must
  not be represented as production access. The existing Play draft AAB targets production and
  cannot validate staging-signed grants. Validate a Play-distributed production build separately.

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
