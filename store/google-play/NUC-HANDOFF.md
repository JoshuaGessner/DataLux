# NUC work to resume before paid Play testing

> **Status:** Inspection resumed November 4; billing and off-device backups remain pending
> **Owner:** Accounts / release engineering
> **Last reviewed:** 2026-10-29
> **Related:** [Upload readiness](UPLOAD-READINESS.md), [Data Safety](DATA-SAFETY.md)

On November 4 the live config had no billing settings or license signer, and the purchase table
contained zero purchases. A new P-256 signer was created under the private DataLux secrets directory;
its public/private key ID `4b72019e9420f53f` matched and its public key was configured in the
GitHub `play-internal` environment. Production billing is still not configured. Preserve this signer;
an encrypted off-device backup of it and the separate Play upload key remains outstanding.

The managed backup command created a verified account snapshot at
`/home/josh/.local/share/datalux/backups/20261105T015751Z-2b476e`. It restored successfully to
a separate temporary volume and passed a schema 4 check; the test volume was removed. This account
snapshot does not include the license signer. Retention and off-device recovery still need review.

## Reconnect and inspect

Use the existing account `josh@192.168.0.179` and managed deployment last verified at
`/home/josh/serv/DataLux-Web`. If a temporary master connection is needed, the owner can leave
`ssh -M -S ~/.ssh/fileflow-nuc.sock josh@192.168.0.179` open. Inspect the actual configuration,
release/image digest, database schema, mounts, disk space and backup state before any changes.
Keep the existing volume/controller paths and preserve mail/admin credentials.

## License trust — blocks the release AAB

1. Verify a secure off-device recovery backup of the newly created signer and the distinct Play
   upload keystore. Never replace a production identity merely to unblock a build.
2. Confirm `FILEFLOW_LICENSE_PUBLIC_KEY` in `play-internal` continues to match the signer's public
   key ID above. `FILEFLOW_ACCOUNT_ORIGIN` is the production DataLux HTTPS origin.
3. Verify the final AAB's key ID against this signer. No disposable test key should become the
   production trust anchor. The separate Android upload keystore is already provisioned locally.

## Backend release and billing

1. Take a fresh verified backup and rehearse restoration to a separate volume. Record the old
   image/configuration before deploying the reviewed backend using the managed release process.
2. Deploy the package-aligned backend: Publisher requests/acknowledgements and notification
   filtering must use `dev.datalux`; the Pro product remains `fileflow_pro`. Main source contains
   this change. Reinspect the current deployment and verify actual package behavior before enabling billing.
3. The owner completed the payments profile October 29. Console requires the first billing-enabled
   artifact before product creation; the release manifest already declares BILLING. Once the verified
   signer enables the real candidate upload and `fileflow_pro` creation, configure least-privilege Publisher credentials
   read-only in the backend. Keep billing disabled until all required secrets exist.
4. Set up authenticated one-time/voided purchase notifications, exact public OIDC audience and
   expected service-account identity. Review quotas/cost controls before enabling any Cloud service.
5. Create a separate staging database/origin and test signer for Google's license-test purchases.
   Production must reject test receipts. Never enable the test-purchase bypass on production.
   The uploaded code 5 AAB points at production and trusts the production key; it cannot validate
   staging grants. A same-package debug-signed staging build may be sideloaded for enrolled Google
   license testers, with the staging HTTPS origin and public key compiled in. Keep staging accounts
   and grants separate from production. Later validate the signed production-origin Play build too.
6. Test real account/email flows, purchase/acknowledgement, pending/cancelled payment, restore,
   duplicate delivery, account mismatch, refund/revocation, offline access and notification retries.
   Include free desktop → Free/Pro phone and encrypted batches; confirm no partial batch save.

## Privacy, deletion and recovery

- Inspect real proxy/host logs and account-backup settings for the Data Safety declaration.
- Proposed launch baseline: expire routine account backups after **30 days**, subject to storage
  and restore review. This is a proposal, not an implemented/public retention promise.
- Implement/rehearse expiry and deletion after restore before publishing the retention period.
  Restoring a database must not silently recreate deleted accounts or purchase access.
- Verify encrypted purchase-token/admin-key recovery together, plus the separate license signer.
- Test both in-app account deletion and the external deletion page. Preserve user-owned device files.
- Provision and rehearse a dedicated verified reviewer account with legitimate Pro access; never
  send owner/admin credentials or fabricate a Google purchase to satisfy review.

## Acceptance record

Record the deployed source/image, backup/restore result, signer key ID, purchase test results,
notification verification, retention policy and operator/date here when actually completed.
Until then these are pending tasks, not release evidence.
