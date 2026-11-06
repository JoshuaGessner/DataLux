# DataLux Data Safety review draft

> **Status:** Draft; not submitted; validate against the final signed build and deployment
> **Owner:** Product / privacy
> **Source inventory refreshed:** 2026-11-04; deployment and final AAB still need review
> **Related:** [Upload readiness](UPLOAD-READINESS.md), [product policy](../../PRODUCT.md)

This is the evidence worksheet for the owner completing Play Console, not a claim that a
declaration has been accepted. Google's form covers all versions/regions distributed through
Play. Review the merged release manifest, dependencies, actual server logs and providers before
submission. Accounts and billing make a blanket “no data collected” answer inaccurate.

## Data inventory

| Data | Actual flow | Draft form treatment / purpose |
| --- | --- | --- |
| Name, email | Optional account registration/management; stored on the account server; email provider sends verification/recovery | Personal info: name and email; optional collection; account management |
| Account UUID and account-bound purchase identifier | Used by native account API and Google purchase binding | Personal info: user IDs; optional collection; account management and fraud prevention/security |
| Purchase history | Purchase token verified with Google, encrypted on server, entitlement/acknowledgement state retained | Financial info: purchase history; optional collection; app functionality, account management and fraud prevention/security |
| Password | Sent over HTTPS to authenticate; only a password hash is retained server-side | Authentication information; do not claim passwords never leave the device. Map to the current Console form's account-data categories |
| Network address / security request metadata | Temporarily used for abuse limits; reverse proxy/host logging depends on deployment | Inspect real logging/retention before final answers. Declare any retained identifiers or derived location actually used; do not invent a no-logging claim |
| Transferred files, camera frames, pairing/team keys | Optical transfer and local processing; no upload to the account service | Not collected by the developer for ordinary transfers. User-directed optical/share-sheet transfers need review against Google's user-initiated sharing exception |
| Local diagnostics | Session logs are saved in app-private files and logcat; send logs can include selected filenames, batch filenames and sizes. The device-check report can be shared through the Android share sheet. A debuggable build can save raw camera frames locally, including automatically during capture; see the build distinction below | No automatic diagnostic upload was identified in the reviewed native source. Local storage alone does not establish Play data collection, but any support reports that the operator receives require review against the form and the actual support workflow |
| Payment-card information | Google Play's payment interface | DataLux does not receive card details. Apply Google's payment-provider exception only to information DataLux does not receive; purchase history above is still collected |

There is no advertising SDK, native third-party analytics SDK, location permission, contacts
permission, microphone permission or broad photo-library permission in the source manifest.
Confirm the final merged manifest/dependency report rather than relying on this source inventory.
The Billing library is `com.android.billingclient:billing:9.1.0`; review the final SDK/provider
behavior. Do not infer zero data collection merely because camera/file processing is local.

The website's first-party page/click analytics are described separately in the public privacy
policy. Account pages omit that reporting. The native app opens external account pages; if an
embedded web view or native telemetry is added later, reassess this worksheet.

## Evidence and build boundaries (2026-11-04)

This source review covers native `914f1755ba2c06277d9eda1ccbf687448b4b1dbe` and web
`518b37299bb1292ae1752ed7f24f86cbfb6fe1e6`; it is not evidence that either commit is
deployed or that the final Play artifact matches them. The later native changes add camera
guidance and capture diagnostics; the web changes add local save/recovery and camera handling.
Recheck the final signed candidate and deployed server before completing the form.

- `app/src/main/AndroidManifest.xml` declares CAMERA and INTERNET and prohibits cleartext;
  the Play billing permission is added through the merged manifest. The final merged manifest
  and transitive dependency set, including Google Play Billing, must still be checked.
- `SessionLog.kt` keeps up to 20 session logs in app-private storage and also writes to logcat;
  `TransmitActivity.kt` writes names and sizes, and `ProbeActivity.kt` can share a device-check
  report using the system chooser. Treat diagnostic evidence as potentially sensitive even if
  file contents are not sent to the account service.
- `FrameDump.kt` gates raw camera-frame dumps on Android's debuggable flag. In a **debug APK**,
  `CaptureActivity.kt` can request dumps automatically on a colour stall or first good data as
  well as manually. Frames can include the scene surrounding a screen and persist locally.
  These debug captures are not evidence of a Play-release behavior: confirm the signed AAB is
  non-debuggable and test the actual release. Ask experimental testers to use synthetic data and
  non-sensitive surroundings, and do not solicit raw frames or logs in a public forum.
- The web recovery path adds temporary in-memory copies and user-requested browser downloads
  when local storage fails (`web/app.js`, `web/buffers.js`). This source change does not establish
  that the updated website is live or independently prove every deployment's handling.

## Open verification before Console submission

1. Compare the reviewed AAB's merged manifest and dependency report with the inventory above;
   verify its `debuggable` and backup settings and repeat the SDK/provider data review.
2. Inspect the deployed account service, proxy and hosting configuration for request logs,
   security metadata, processing purposes, retention and actual third-party recipients.
3. Exercise account deletion in the exact released app and external deletion page. Verify backup
   expiry and deletion-after-restore rather than assuming active database deletion covers backups.
4. Confirm the support intake and reviewer-access flow, and review what information is actually
   shared with Gmail, Brevo and Google under the applicable Play definitions.
5. Compare the final store listing and privacy policy with tested behavior, including local
   diagnostics and the current paid-feature availability; submit only verified declarations.

## Security and deletion evidence

- Native account traffic requires HTTPS, validates platform certificates, and does not follow
  redirects with account credentials. Android explicitly refuses cleartext traffic.
- Android's cached account file uses a device-keystore key in `noBackupFilesDir`. Version 0.4.1
  also disables app-data cloud backup and excludes every app storage domain from cloud and
  device migration, protecting pairing/team state and local diagnostics. This does not control
  a user's separate backup of public Downloads or exported files.
- Users can request deletion inside Pro/account settings and at
  `https://datalux.anomalousinteractive.com/account.html#delete` after signing in and confirming
  their password. Test both paths against the deployed service before submission.
- Active-service deletion scrubs linked audit PII and account/purchase records. Existing files
  on devices remain. Offline grants may persist without reconnecting; do not promise remote erasure.
- **Unresolved release gate:** verify and publish a specific server-backup expiry period and
  rehearse deletion after restore. Do not mark backup erasure complete on the basis of active-DB deletion.
- Service-provider sharing exceptions depend on the actual Brevo/hosting/Google arrangements;
  verify them before choosing “not shared.” Do not substitute this worksheet for those facts.

## Console fields to complete after evidence review

Privacy URL: `https://datalux.anomalousinteractive.com/privacy.html`.
Deletion URL: `https://datalux.anomalousinteractive.com/account.html#delete`.
Support: `dataluxsupport@gmail.com`.

Confirm encryption in transit, optional account collection, deletion availability and each data
purpose. Answer rating/target audience from the actual general-purpose utility experience;
do not classify the app as directed to children without a deliberate product/policy change.
No ads are implemented; Pro is an in-app purchase. Keep account/reviewer access separate from
the privacy declaration. Final submission remains owner-controlled.

## Primary references

- [Google's Data Safety definitions, exceptions and payment-provider guidance](https://support.google.com/googleplay/android-developer/answer/10787469)
- [Account deletion requirements](https://support.google.com/googleplay/android-developer/answer/13327111)
- [Android backup defaults and extraction rules](https://developer.android.com/identity/data/autobackup)
- [Android data-use inventory guidance](https://developer.android.com/privacy-and-security/declare-data-use)
