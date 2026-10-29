# DataLux rename and deployment

DataLux is the new public product name. The account origin is `https://datalux.anomalousinteractive.com`. Desktop and Android builds default to that origin; existing CMake caches and GitHub variables can override the defaults. Changing origins requires signing in again and does not transfer browser-local files.

## Compatibility

- The owner created the DataLux Play entry and confirmed application ID **`dev.datalux`** on October 29. Android builds, launcher shortcut targets, bundle validation, Publisher verification and notification filtering use that ID together. The Kotlin namespace/classes and JNI bindings stay `dev.fileflow`; internal action names, key aliases, protocol/license formats and the Google Play product `fileflow_pro` remain unchanged.
- **Android installation boundary:** `dev.datalux` is a separate app from historical `dev.fileflow` builds, even if the same signing key is used. It does not replace or automatically inherit the old app's private account, pairing, team or diagnostic state. Preserve user files, sign in to the new app and recreate pairing/teams as needed. Public files in Downloads are user-owned; never uninstall or erase the old app as part of this source migration. Pro restoration still requires a legitimate account entitlement and configured billing/trust.
- New desktop builds use `DataLux.app` and `DataLux.exe`. The macOS bundle ID, desktop preference location, Linux `fileflow-send`, legacy license extension and `Downloads/FileFlow` remain stable. Installing the new macOS bundle beside an older one can leave both copies on disk.
- Pairing and encryption domain-separation strings, license payloads, server encryption contexts, account database, cookies, local storage, API paths, configuration keys and `FILEFLOW_*` environment variables are contracts. Change them only with a versioned, tested migration.
- Historical FileFlow release assets, filenames, checksums, signatures and manifest URLs remain unchanged. GitHub repositories are `JoshuaGessner/DataLux` and `JoshuaGessner/DataLux-Web`; the release catalog accepts both old and new native asset paths. The release manifest identifier remains unchanged, and historical web images retain their original registry path. Historical repair workflows pin the old repository identity and fail closed after the rename; update and review them separately before any future repair.
- The managed production service, project, volume, network, registry image and configuration directory use DataLux names. The old deployment, volume and configuration are retained for rollback. The email sender identity is independent of the website hostname and must be verified before changing it.

## Deployment and rollback

See the private web repository's deployment runbook for controller commands. Before an update, inspect the actual server state, take and verify a fresh backup, rehearse restoration to a separate volume, and record the previous release and image digest. Review the exact source commit and publish an immutable image using the owner-controlled release workflow. Deploy the approved digest with the existing volume; verify HTTPS, readiness, account access, emails, downloads and native clients.

The controller's `reconfigure` command restarts the existing image using the current configuration. Back up the configuration before changing the origin. A failed backup can still restart using the new configuration; restoring the previous image alone does not restore the old origin. Restore the prior configuration and follow the controller's pending-operation recovery instructions if rollback is required.

The website's account server enforces a single configured browser origin. Cookies and IndexedDB are origin-specific, and database backups do not include browser-local files. Export local files before retiring an old hostname if they matter. Old native builds may still target the old hostname; plan their replacement or a tested compatibility route.
