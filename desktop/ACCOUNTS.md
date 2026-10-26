# Desktop accounts

The Account tab supports browser-assisted sign-in. The PC requests a ten-minute code, opens the configured FileFlow website, and polls for approval no more often than every five seconds. The website requires a signed-in, email-verified account and explicit approval. Passwords stay in the browser; the device secret and session bearer never appear in URLs or application logs.

Account traffic is separate from optical transfer. No network request starts until the user signs in, or a previously signed-in account is refreshed. HTTPS uses the system trust store: Foundation/NSURLSession on macOS, WinHTTP on Windows, and libcurl 7.85+ on Linux. Redirects are disabled, responses are bounded to 32 KiB, and requests have bounded timeouts. A Linux runtime needs libcurl and the system CA certificates. Camera frames, transfer files, teams, and pairing keys are never sent to the account API.

The account session and signed Pro grant are cached in `account-session.json` under the existing `SDL_GetPrefPath("FileFlow", "Desktop")` directory. Its bearer credential is stored in a private file using the existing atomic writer (0600 on Unix; owner-only ACL on Windows). **This file is not encrypted by an OS credential vault.** It must be kept private and excluded from shared support bundles. The stored origin must match the build's account origin; a staging session is not sent to another origin. Native OS keychain integration remains an optional hardening improvement.

The cached Pro license is cryptographically verified against the build-pinned public key and the signed-in account ID. A normal refresh runs at startup for previously signed-in accounts and at most every six hours after success. Network or authentication errors keep existing offline Pro access. Expired/rejected sign-in offers reauthentication. An authoritative Free result changes entitlement only when no send, receive, preparation, or pairing operation is active. Signing out also requires an idle app and clears only local account/Pro activation; it leaves files, settings, recent history, teams, and pairing data intact. Server-session revocation is best effort while offline.

Manual `.fileflow-license` import remains available as a fallback. While signed in, an imported grant must belong to the same account. Source/community builds still include every feature independently of accounts.

Build inputs:

- `FILEFLOW_ACCOUNT_ORIGIN`: HTTPS origin, default `https://fileflow.anomalousinteractive.com`; no path, credentials, or query. Use a distinct staging hostname and signing key for internal tests.
- `FILEFLOW_STORE_BUILD=ON`: explicitly gate batch sending for a paid distribution build. Default OFF.
- `FILEFLOW_LICENSE_PUBLIC_KEY_FILE`: the PEM SPKI **public** license verification key. Never supply the private signing key to a client build.

The local macOS build and deterministic parser/signature/storage tests are verified. Windows and Linux transport adapters require their CI builds and real HTTPS sign-in acceptance on those platforms before release. The API must be deployed before browser sign-in will function; existing public releases do not gain these changes automatically.
