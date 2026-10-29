# Genuine Android screenshot captures

Captured on 2026-10-29 from the installed DataLux app on a Samsung SM-S948U1 running Android 17. These are direct device PNG captures, without compositing, cropping, pixel edits, fabricated files, or transfer results.

## Build provenance

- Package: `dev.datalux`
- Version: `0.4.1` (version code `5`)
- Build: local **debug** APK; these images are UI evidence, not proof of a Play-signed production installation.
- Source commit provided for this APK: `edccb3c44c72d81cf67a1d9cee019c26801db1ba`
- APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK SHA-256: `26e585f6adc4fc2817508339d69b599f28352a72a6c9dee9701e6dd4d99ed6e5`

## Images

Every PNG is 1080 × 1920 pixels (9:16).

| File | Actual screen | Review note |
| --- | --- | --- |
| `01-send.png` | Send home, free single-file and Pro batch explanation | Suitable candidate. |
| `02-settings.png` | Sending speed, compression, Teams and Pro settings | Scrollable screen; the Pro card continues below the bottom navigation. |
| `03-pro.png` | Free/Pro comparison and US $10 one-time Android upgrade explanation | Suitable candidate. No credentials entered and no purchase attempted. |
| `04-received.png` | Empty Received library | Shows the real legacy `Downloads › FileFlow` folder. Review rebrand presentation before using this image. |
| `05-teams.png` | Empty Teams view | Uses the generic default phone model name. The Join button continues below the visible scroll area. |
| `06-receive.png` | Receive home before camera permission or capture | Suitable candidate. No camera permission granted and no transfer result implied. |

The screenshots include ordinary Android status icons and the Samsung edge-panel handle. No notification panel was opened, personal notifications dismissed, or screenshot pixels edited. The main agent visually reviewed the captures and attached `02-settings.png`, `01-send.png`,
`06-receive.png` and `03-pro.png` to the US listing on October 29. Console accepted the listing
as **Ready to send for review**; it has not been sent for Google app review. Recheck these images
against the final signed candidate before submission. The device captures carry no AI asset label.

## Capture and device checks

- `dev.datalux` was absent for the primary Android user before this debug build was installed. Installation succeeded; launch and navigation through Send, Receive home, Settings, Pro, Received and Teams were visually checked.
- The old app package, watch, existing user files and account state were not changed. No sign-in, purchase, file selection, team creation, or permission grant was performed.
- Camera permission remained `granted=false` after the checks.
- The physical display is 1080 × 2340 with density 450 and originally had no size or density override.
- Each listing capture temporarily requested `wm size 1080x1920`, captured the actual responsive UI, and restored the exact prior size override in a Python `finally` block. Final `wm size` again reported only `Physical size: 1080x2340`; density was never changed.
- No demo-mode, notification, Do Not Disturb or other global presentation setting was changed.
- Runtime `getconf PAGE_SIZE` returned `4096`. This phone does not provide 16 KB page-size runtime evidence; this observation does not satisfy the signed-build/native-camera release gate.
- No crash or blocking visual error was observed in this bounded navigation check. Optical transfer, camera receiving, paid-feature enforcement across devices, purchase/restore/refund handling and production signing remain separate release tests.
