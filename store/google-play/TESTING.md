# DataLux Play test guide

> **Status:** Planned acceptance tests; no passes recorded here
> **Owner:** Release engineering / test coordinator
> **Last reviewed:** 2026-10-29
> **Related:** [Product policy](../../PRODUCT.md), [upload readiness](UPLOAD-READINESS.md), [deferred NUC work](NUC-HANDOFF.md)

The candidate is **0.4.1 / version code 5**, package **`dev.datalux`**, Pro product
**`fileflow_pro`**. Recheck the uploaded version before testing. A debug install, CI pass or
static alignment report does not establish Play billing, optical transfer or device acceptance.
The release AAB still requires the verified production license public key. NUC deployment,
private signer inspection and server acceptance remain deferred until the owner is home.

## Test groups and sequence

1. Start with an **internal track** for the owner and a small trusted group. Install the exact
   reviewed Play build, using the Google account enrolled in that track. Preserve files before
   replacing a debug APK: a different signing certificate may require uninstalling, which removes
   app-local state. Historical `dev.fileflow` is a separate app; do not wipe it to test DataLux.
2. Add billing participants separately as **license testers**. Track membership alone does not
   prevent real charges. Check the purchasing Google account and the explicit test-payment notice
   on Google's checkout sheet; stop if it is absent. Use approved test methods only.
3. Test billing against the isolated staging setup described in the NUC handoff. Record its origin
   and trust-key ID privately. Production must reject test receipts; never enable a production
   test-purchase bypass or substitute a disposable signer for the production trust anchor.
4. After app setup and initial acceptance, start the qualifying **closed test**. The Personal
   account needs at least **12 testers continuously opted in for 14 days** before applying for
   production access. Internal testing does not count. A tester moving from internal to closed
   must opt out of internal first, then opt into closed and confirm the installed track/build.
5. Collect actual use, problems, fixes and feedback throughout the closed test. Keep enough
   participating testers to avoid dropping below 12; record the Console eligibility date rather
   than assuming that adding an email address starts the clock. Do not promise a production date
   until Google grants access and completes release review.

Google references: [test tracks](https://support.google.com/googleplay/android-developer/answer/9845334),
[billing test methods](https://developer.android.com/google/play/billing/test),
[Personal-account requirements](https://support.google.com/googleplay/android-developer/answer/14151465).

## Safe fixtures and setup

Use a dedicated local test folder containing `hello.txt` (a short public sentence), a second
text file, and a small generated solid-colour PNG. Record byte counts and SHA-256 hashes before
sending. Keep payloads small for the first optical test, then test supported larger payloads
within documented limits. Use synthetic filenames/content, dedicated test accounts and no
personal photos, documents, purchase tokens or pairing keys in shared evidence.

Prepare a desktop sender, an Android Free account/state and a legitimately activated Android
Pro account/state. A second phone is useful for restore and phone-to-phone checks. Record sender
and receiver versions, device models, Android versions, screen refresh/brightness, selected
profile, lighting and distance. Do not treat the experimental browser sender as the default
reviewer path without rehearsing that exact path.

## Acceptance matrix

Every row starts **NOT RUN**. Mark PASS only after recording expected and actual results for the
exact candidate. Mark BLOCKED with a dependency when hardware, Play setup or NUC access is absent.
Run transfer rows in both directions where applicable and compare recovered hashes, not just UI.

| ID | Exercise | Required result |
| --- | --- | --- |
| FREE-01 | Signed-out phone sends and receives one synthetic file without network | Works without account or purchase; recovered hash matches |
| FREE-02 | Free speed/compression choices, pairing/team encryption and individual file open/share/resend/delete | Available without Pro; no artificial daily quota or speed paywall |
| DESK-01 | Desktop batch, teams and presenter/multiple-display controls while signed out | Desktop features remain free; no activation requirement |
| BATCH-01 | Desktop sends two files to Free phone, plain then encrypted | Receiver offers Pro/restore or one-file alternative; saves **no batch members**; existing files remain |
| BATCH-02 | Licensed phone sends batch to Free phone | Receiving phone still enforces its own entitlement; sender's Pro does not unlock it |
| BATCH-03 | Desktop sends same batch to Pro phone, plain then encrypted | Both files saved exactly once with matching hashes; no partial or corrupt result |
| BATCH-04 | Receive single-file legacy batch and normal single file | Free remains usable; advisory alone never makes a single file paid |
| UPGRADE-01 | Open upgrade early during scanning, then cancel/return | No automatic purchase; scanner/camera stops for checkout; explain/restart the required new scan |
| UPGRADE-02 | Complete blocked batch; activate/restore while receive screen survives | Explicit **Save batch after upgrade** saves verified batch without retransmission; closing screen/process requires a new receive |
| BULK-01 | Free and Pro received-file multi-select/share/resend/delete | Free retains individual actions and offers upgrade for bulk; Pro bulk works only on intended files |
| PAY-01 | Approved one-time test purchase | Backend verifies correct package/product/account, acknowledges, issues one account-bound grant; no duplicate charge/unlock on callback retry |
| PAY-02 | Pending, declined and cancelled test purchase; restart app | No Pro while pending/failed/cancelled; eventual approval unlocks only after verification |
| PAY-03 | Restore on another phone using same verified DataLux account; repeat restore | Legitimate ownership restored without another purchase; repeated requests are safe |
| PAY-04 | Sign into another DataLux account, wrong-account token, invalid/replayed receipt | No cross-account grant or accidental entitlement transfer; useful error, no secret disclosure |
| PAY-05 | Refund/revoke, duplicate/delayed/authenticated notification and acknowledgement failure | Reconciliation stops issuing revoked grants; retries are safe; forged notifications rejected; inspect actual backend result |
| OFFLINE-01 | Activated Pro offline; Free offline; refund/delete while another device stays offline | Cached valid Pro and Free optical transfers work. Record perpetual-grant limit: offline revocation is not immediate; never erase user files to enforce it |
| ACCOUNT-01 | Verification, recovery, sign-out and in-app/external deletion using disposable test account | Exact production-intended paths work; deletion scrubs active account/purchase data; device files remain; backup expiry/deletion-after-restore verified separately on server |
| DEVICE-01 | Cold launch, camera denial/retry, scanning interruption, background/resume, corrupt/incomplete transfer | No crash or invalid file save; permission and retry behavior understandable; no unexpected camera activity after leaving receive |
| PAGE-01 | Exact native build on verified 16 KB runtime | Record page size **16384**, launch, native encode/decode and physical optical transfer; no compatibility-only claim from ELF/ZIP alignment |
| REVIEW-01 | Fresh install and exact private reviewer instructions | Free transfer and legitimate Pro access reproducible without developer OTP, personal/admin credentials or reviewer payment |

For PAGE-01, record the runtime's actual page size (for example `adb shell getconf PAGE_SIZE`
on the specifically selected test device). Android version alone does not prove 16 KB pages.
A supported 16 KB emulator can test native launch/processing; physical camera transfer remains
separate evidence. See [Android's 16 KB test guidance](https://developer.android.com/guide/practices/page-sizes).

## Record results without secrets

Copy this record per test into the designated private test report. A sanitized issue can be
published only when it contains no private identifiers or content.

```text
Test ID / date / tester alias:
Status: NOT RUN | BLOCKED | PASS | FAIL
App version + code / source SHA / artifact SHA-256:
Install source + track / backend environment label + public key ID:
Sender + receiver model / OS / runtime page size:
Account state: signed out | Free | Pro (no address, password or token)
Fixture names + byte counts + hashes:
Steps / expected result / actual result:
Evidence path or private issue / reproducibility / blocking dependency:
```

Before promoting the candidate, archive its review report, source SHA, bundle hash, public signing
certificate, license public-key ID and exact test results. Resolve release-blocking failures,
confirm screenshot/copy accuracy, rehearse reviewer access and update [upload readiness](UPLOAD-READINESS.md)
with completed evidence. Keep private keys, passwords, purchase receipts and account exports out
of Git, public artifacts, screenshots and routine bug reports.
