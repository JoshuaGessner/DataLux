# NUC work to resume before paid Play testing

> **Status:** Deferred until the owner is back on the NUC network
> **Owner:** Accounts / release engineering
> **Last reviewed:** 2026-10-29
> **Related:** [Upload readiness](UPLOAD-READINESS.md), [Data Safety](DATA-SAFETY.md)

The owner explicitly deferred NUC work on October 29 because this computer is off its network.
Do not keep retrying SSH, rotate an uninspected signer or claim production billing is configured.
The public DataLux readiness endpoint responds successfully; private configuration remains unverified.

## Reconnect and inspect

Use the existing account `josh@192.168.0.179` and managed deployment in
`/home/josh/serv/FileFlow-Web`. If a temporary master connection is needed, the owner can leave
`ssh -M -S ~/.ssh/fileflow-nuc.sock josh@192.168.0.179` open. Inspect the actual configuration,
release/image digest, database schema, mounts, disk space and backup state before any changes.
Keep the existing volume/controller paths and preserve mail/admin credentials.

## License trust — blocks the release AAB

1. Inspect whether a permanent P-256 license signer already exists or has issued any grants.
   Reuse it and verify its backup. Never replace an identity merely to unblock a build.
2. If no production signer exists, create the permanent signer in private server storage using
   the existing key utility and make a secure recovery backup. Keep the private key on the server;
   only its SPKI DER public key and key ID are needed by Android/CI.
3. Configure GitHub `play-internal` variable `FILEFLOW_LICENSE_PUBLIC_KEY` with that verified
   public key. `FILEFLOW_ACCOUNT_ORIGIN` is already the production DataLux HTTPS origin.
4. Verify the final AAB's key ID against this signer. No disposable test key should become the
   production trust anchor. The separate Android upload keystore is already provisioned locally.

## Backend release and billing

1. Take a fresh verified backup and rehearse restoration to a separate volume. Record the old
   image/configuration before deploying the reviewed backend using the managed release process.
2. Deploy the package-aligned backend: Publisher requests/acknowledgements and notification
   filtering must use `dev.datalux`; the Pro product remains `fileflow_pro`. Main source contains
   this change, but it has not been deployed or verified on the NUC during the off-network session.
3. After merchant setup and product creation, configure least-privilege Publisher credentials
   read-only in the backend. Keep billing disabled until all required secrets exist.
4. Set up authenticated one-time/voided purchase notifications, exact public OIDC audience and
   expected service-account identity. Review quotas/cost controls before enabling any Cloud service.
5. Create a separate staging database/origin and test signer for Google's license-test purchases.
   Production must reject test receipts. Never enable the test-purchase bypass on production.
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
