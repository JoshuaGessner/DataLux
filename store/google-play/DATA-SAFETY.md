# DataLux Data Safety review draft

> **Status:** Draft; not submitted; validate against the final signed build and deployment
> **Owner:** Product / privacy
> **Last reviewed:** 2026-10-29
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
| Local diagnostics | Local logs/frame diagnostics; exported only by the user | No automatic crash/diagnostic uploader in this build. Review any support export workflow before asserting diagnostics are never received |
| Payment-card information | Google Play's payment interface | DataLux does not receive card details. Apply Google's payment-provider exception only to information DataLux does not receive; purchase history above is still collected |

There is no advertising SDK, native third-party analytics SDK, location permission, contacts
permission, microphone permission or broad photo-library permission in the source manifest.
Confirm the final merged manifest/dependency report rather than relying on this source inventory.
The Billing library is `com.android.billingclient:billing:9.1.0`; review the final SDK/provider
behavior. Do not infer zero data collection merely because camera/file processing is local.

The website's first-party page/click analytics are described separately in the public privacy
policy. Account pages omit that reporting. The native app opens external account pages; if an
embedded web view or native telemetry is added later, reassess this worksheet.

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
Support: `fileflowsupport@gmail.com` (existing verified contact; no new inbox is implied by the rename).

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
