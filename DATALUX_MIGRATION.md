# DataLux rename and deployment

DataLux is the new public product name. The account origin is `https://datalux.anomalousinteractive.com`. Desktop and Android builds default to that origin; existing CMake caches and GitHub variables can override the defaults. Changing origins requires signing in again and does not transfer browser-local files.

## Compatibility

- Android retains application ID and namespace `dev.fileflow`, JNI bindings, signing identity, keystore aliases and Google Play product `fileflow_pro`. Changing an application ID creates a separate app and requires a separately planned migration.
- New desktop builds use `DataLux.app` and `DataLux.exe`. The macOS bundle ID, desktop preference location, Linux `fileflow-send`, legacy license extension and `Downloads/FileFlow` remain stable. Installing the new macOS bundle beside an older one can leave both copies on disk.
- Pairing and encryption domain-separation strings, license payloads, server encryption contexts, account database, cookies, local storage, API paths, configuration keys and `FILEFLOW_*` environment variables are contracts. Change them only with a versioned, tested migration.
- Historical FileFlow release assets, filenames, checksums, signatures and manifest URLs remain unchanged. GitHub repositories are `JoshuaGessner/DataLux` and `JoshuaGessner/DataLux-Web`; the release catalog accepts both old and new native asset paths. The release manifest identifier and container registry path remain unchanged. Historical repair workflows pin the old repository identity and fail closed after the rename; update and review them separately before any future repair.
- The managed production volume, network, deployment controller and configuration paths remain unchanged. The email sender identity is independent of the website hostname and must be verified before changing it.

## Deployment and rollback

See the private web repository's deployment runbook for controller commands. Before an update, inspect the actual server state, take and verify a fresh backup, rehearse restoration to a separate volume, and record the previous release and image digest. Review the exact source commit and publish an immutable image using the owner-controlled release workflow. Deploy the approved digest with the existing volume; verify HTTPS, readiness, account access, emails, downloads and native clients.

The controller's `reconfigure` command restarts the existing image using the current configuration. Back up the configuration before changing the origin. A failed backup can still restart using the new configuration; restoring the previous image alone does not restore the old origin. Restore the prior configuration and follow the controller's pending-operation recovery instructions if rollback is required.

The website's account server enforces a single configured browser origin. Cookies and IndexedDB are origin-specific, and database backups do not include browser-local files. Export local files before retiring an old hostname if they matter. Old native builds may still target the old hostname; plan their replacement or a tested compatibility route.
