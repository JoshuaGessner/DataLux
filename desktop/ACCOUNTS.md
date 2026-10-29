# Desktop accounts

> **Status:** Accepted; production Play acceptance pending
> **Owner:** Desktop / accounts
> **Last reviewed:** 2026-10-28
> **Related:** PRODUCT.md, NATIVE_RELEASES.md

All DataLux desktop features are free, including batch sending, bulk received-file actions,
compression, pairing, encryption, and teams. No sign-in, license, purchase, or network connection is
required to use them. Android Pro is a separate $10 one-time upgrade for phone batch sending and
receiving and bulk received-file actions. A Free Android phone receiving a desktop batch must
upgrade or ask the sender to send individual files; the desktop sender itself is never paywalled.

The Account tab supports browser-assisted sign-in. The PC requests a ten-minute code, opens the configured DataLux website, and polls for approval no more often than every five seconds. The website requires a signed-in, email-verified account and explicit approval. Passwords stay in the browser; the device secret and session bearer never appear in URLs or application logs.

Account traffic is separate from optical transfer. No network request starts until the user signs in, or a previously signed-in account is refreshed. HTTPS uses the system trust store: Foundation/NSURLSession on macOS, WinHTTP on Windows, and libcurl 7.85+ on Linux. Redirects are disabled, responses are bounded to 32 KiB, and requests have bounded timeouts. A Linux runtime needs libcurl and the system CA certificates. Camera frames, transfer files, teams, and pairing keys are never sent to the account API.

The account session and signed Pro grant are cached in `account-session.json` under the existing `SDL_GetPrefPath("FileFlow", "Desktop")` directory. Its bearer credential is stored in a private file using the existing atomic writer (0600 on Unix; owner-only ACL on Windows). **This file is not encrypted by an OS credential vault.** It must be kept private and excluded from shared support bundles. The stored origin must match the build's account origin; a staging session is not sent to another origin. Native OS keychain integration remains an optional hardening improvement.

Account sign-in is optional and lets a person inspect or manage the account used for Android Pro.
Cached Pro records are cryptographically verified against the build-pinned public key and the
signed-in account ID; they never grant or remove desktop features. A normal refresh runs at startup
for previously signed-in accounts and at most every six hours after success. Expired/rejected
sign-in offers reauthentication. Account changes are applied only when no send, receive, preparation,
or pairing operation is active. Signing out also requires an idle app and clears only local
account/activation state; it leaves files, settings, recent history, teams, and pairing data intact.
Server-session revocation is best effort while offline.

Legacy `.fileflow-license` imports remain readable through the existing file/CLI path for
compatibility. While signed in, an imported grant must belong to the same account. The Account tab
does not offer activation because desktop features are already available. Existing private session
and activation files retain their format and location.

Build inputs:

- `FILEFLOW_ACCOUNT_ORIGIN`: HTTPS origin, default `https://datalux.anomalousinteractive.com`; no path, credentials, or query. Use a distinct staging hostname and signing key for internal tests. Existing CMake caches retain their prior value; set it explicitly or configure a fresh build directory to adopt the new default.
- `FILEFLOW_STORE_BUILD`: deprecated and ignored. Even a previously configured `ON` cache is reset to
  `OFF`; desktop features always remain free.
- `FILEFLOW_LICENSE_PUBLIC_KEY_FILE`: optional PEM SPKI **public** key for verifying Android Pro
  account records. It is not required for desktop features. Never supply the private signing key to
  a client build.

Parser/signature/storage tests and Windows/Linux CI builds have passed previously. Each new release
still requires its platform build, smoke tests, and relevant account acceptance checks. The account
API must be deployed before browser sign-in functions; an unavailable account service never blocks
desktop transfers. Existing public binaries do not gain new receiving-phone prompts automatically.
