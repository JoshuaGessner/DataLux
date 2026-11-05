# Isolated Pro purchase test setup

Status: staging infrastructure started November 4, 2026; Caddy/TLS, Google credentials, billing,
test purchase and paid-feature acceptance remain outstanding. See [upload readiness](UPLOAD-READINESS.md)
for the current Play candidate and [test cases](TESTING.md) for expected results.

The NUC staging service uses `https://staging.datalux.anomalousinteractive.com`, loopback port
`8788`, Docker project `datalux-staging`, network `datalux-staging-backend` (`172.30.93.0/24`,
gateway `172.30.93.1`) and separate `datalux-staging-data` volume. Health and readiness passed
locally. Its test signer is private under the NUC's DataLux staging secrets directory; its public
SPKI SHA-256 begins `d1ed41f71b4b4156`, distinct from the production signer. It mounts the existing
SMTP configuration read-only; no test email was sent. Billing is disabled.

The staging image is `datalux-staging:39b8f46`, built on the NUC from web revision
`39b8f469a01a80462d5023663369c7dff494074c`. Its health endpoint reported that revision and
schema 4. It includes the acknowledgement retry fix. The debug-signed staging APK is stored at
`build/artifacts/staging-pro-test/datalux-staging-debug.apk` (SHA-256
`babd238318ceeb36941d318b00d17fbffce66b6091668167888de8c95230192b`), built from the local
working tree with staging origin and test public key. It has not been installed or purchase-tested.
The Google Cloud project `datalux-play-billing` has been created and the Google Play Android
Developer API is enabled. The `datalux-play-purchase-verifier` service identity was created;
service-account credentials and Play Console permissions are pending.

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
   test-track membership is separate. Activate the existing draft buy option only when staging,
   verification and the intended test access are ready. An active option and a published app
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
