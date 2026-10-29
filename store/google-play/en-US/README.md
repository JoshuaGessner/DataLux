# DataLux US Google Play materials

> **Status:** Draft; not submitted to Google Play
> **Owner:** Product / release engineering
> **Last reviewed:** 2026-10-29
> **Related:** [PRODUCT.md](../../../PRODUCT.md), [NATIVE_RELEASES.md](../../../NATIVE_RELEASES.md), [DATALUX_MIGRATION.md](../../../DATALUX_MIGRATION.md)

These English (United States) text files are prepared for owner review. They do not establish a
Play app entry, enable checkout or attest that production approval has been granted. The working
release policy is a free Android download and a planned $10 USD one-time US Pro upgrade. Keep
the price qualified until Console configuration and real purchase acceptance are complete.

On October 29 the text, icon, feature graphic and four genuine Android screenshots were saved
to the English (United States) listing. Console reports **Ready to send for review**; nothing
was sent for Google app review or published. The icon and feature graphic carry AI asset labels
for their assisted preparation; direct device screenshots are unlabeled. Debug launch/navigation
were visually checked, while signed-build, optical and billing acceptance remain pending. See
[screenshot provenance](screenshots/README.md) and the [content-rating record](../CONTENT-RATING.md).

## Text fields

| File | Destination | Review condition |
| --- | --- | --- |
| `title.txt` | App name | DataLux, as chosen by the owner |
| `short-description.txt` | Short description | At most 80 characters |
| `full-description.txt` | Full description | At most 4,000 characters; must match the uploaded build |
| `release-notes.txt` | Internal-test release notes | Test coordinator must confirm the purchase-testing instructions |

The owner-confirmed Play package is `dev.datalux`; the retained Pro product is `fileflow_pro`.
Kotlin/JNI classes stay `dev.fileflow`. Historical APKs with the old package install separately;
preserve files and sign in/re-pair rather than assuming private state moves to the new app.
All text is plain text. Do not include this README in a public store field.

## Existing artwork inventory

| Existing source | What it is | Store use |
| --- | --- | --- |
| `desktop/src/icon_art.h` | Canonical code-based light-grid artwork | Source for a Play-specific square icon export |
| `desktop/tools/icon.cpp` (`ff-icon`) | Existing raster/SVG/Android vector exporter | Reuse this artwork/export path; no replacement brand is needed |
| `assets/fileflow-icon.svg` | Shared generated vector artwork; historical filename | Brand reference; Google Play needs a PNG export |
| `app/src/main/res/drawable/ic_launcher_foreground.xml` and `ic_launcher_monochrome.xml` | Existing Android launcher vectors | Installed-app icon; not a store-listing PNG |
| `build/desktop-app/desktop/icon/fileflow.png` | Locally generated 512 × 512 RGBA desktop icon | Reference only: desktop tile has baked rounded corners/margins and the uncompressed file is above the Play size limit; do not upload unchanged |
| `web/assets/web-mobile.jpg`, `web/assets/web-desktop.jpg` | Website/browser preview imagery | Not verified Android screenshots; do not present as the native app |

## Prepared artwork

The following artwork is ready for the owner's visual review and Play upload. These are marketing
assets, not evidence that Google has accepted the app or that a device test has passed.

| File | Dimensions and format | File size | Notes |
| --- | --- | --- | --- |
| `assets/play-icon-512.png` | 512 × 512, RGBA PNG, sRGB | 16,421 bytes | Canonical native grid; opaque full-square background, no outer rounded mask, transparent margins or shadow |
| `assets/feature-graphic-1024x500.png` | 1,024 × 500, RGB PNG, sRGB | 29,095 bytes | DataLux wordmark, existing colour palette and abstract light path; no price badges, rankings or device mockups |
| `assets/play-icon-source.svg` | Editable vector derivative | Not a Console upload | Canonical paths with a full-square background; historical app assets remain unchanged |

Both PNGs were inspected after rendering. The icon's alpha channel is fully opaque, the feature
graphic has no alpha, and both are below 1 MiB. Google applies the final icon mask and shadow.
Suggested feature-graphic alt text: **DataLux carries files from a screen to a camera using light.**

`render-assets.py` regenerates the PNGs using the existing `ff-icon` renderer, the shared Inter
font and Pillow; it does not edit native/web assets. Build `ff-icon` through the desktop CMake
target first if it is absent. With an existing Python environment containing Pillow, run:

```sh
python3 store/google-play/en-US/render-assets.py
```

The script accepts `--icon-exporter /absolute/path/to/ff-icon` for another build directory.
It requires the separate website checkout's `web/assets/InterVariable.ttf`; its license is
`web/assets/Inter-LICENSE.txt`. The generated vector retains the canonical icon paths, while the
native-rendered PNG extends the original tile gradient to fill Play's square.

## Asset requirements and remaining captures

1. **App icon — prepared above:** 512 × 512, 32-bit PNG, at most 1,024 KB; follow the current
   [Play icon specification](https://developer.android.com/distribute/google-play/resources/icon-design-specifications).
2. **Feature graphic — prepared above:** 1,024 × 500, JPEG or 24-bit PNG without alpha. The
   prepared graphic extends the existing colours and artwork without repeating the complete icon.
3. **Phone screenshots — captured and attached:** four actual DataLux debug UI captures are attached.
   Recheck them against the final signed candidate; capture any changed UI again.
   Google requires at least two listing screenshots. Aim for four or more useful 1,080 × 1,920
   portrait images, with no private files, account identifiers or notifications. Keep originals
   and record the app version, source SHA and capture device alongside the captures.
4. **Optional demonstration video — still to capture:** show a continuous, successful screen-to-camera transfer.
   Never substitute a mock screen or video for functional reviewer access.

Suggested screenshot sequence and alt text (adjust to the actual capture):

| Capture | Content | Draft alt text |
| --- | --- | --- |
| 1 | Main Send/Receive screen | DataLux offers sending and receiving files using a screen and camera. |
| 2 | Actual receiver scanning a small test file | DataLux's receiver scans a transfer pattern from another display. |
| 3 | Successful received-file result | DataLux verifies the received file before making it available to open or share. |
| 4 | Free/Pro screen | DataLux compares free individual transfers with Pro batch and bulk workflows. |
| 5, optional | Free phone's actual batch upgrade/restore prompt | A receiving phone offers Pro or restore for a multi-file batch. |

Screenshot JPEG/PNG dimensions must stay within Google's current 320–3,840 px limits, and the
long side must not exceed twice the short side. For this phone listing use 9:16 captures, not
stretched tall-device images. See [Google's preview asset requirements](https://support.google.com/googleplay/android-developer/answer/9866151).

## Reviewer access draft — fill privately in Console

Provide these instructions in Play Console's App access section only after rehearsing the exact
uploaded build. Fill credentials directly in Console; do not put real passwords into source control.

```text
App: DataLux
Package: dev.datalux
Review account email: <dedicated preverified reviewer account>
Review account password: <enter privately in Play Console>
Pro access: <describe the legitimate, rehearsed entitlement supplied to this account>
Companion sender/download URL: https://datalux.anomalousinteractive.com/download.html
Demonstration video URL: <continuous transfer demonstration, when available>

Free functions work without signing in. Open DataLux to send one file or select Receive.
For receiving, run the DataLux desktop companion on a second display, send the supplied small
test file, allow camera permission on the phone and keep the complete pattern in view.
Check the successful result and open the received file.

For Pro review, open the Pro/account screen and sign into the dedicated reviewer account above.
Use only the tested restoration steps supplied here. Do not require reviewers to purchase Pro
or wait for a verification code from the developer. Send a small multi-file batch from desktop
and verify both files, then exercise the received-file bulk actions.

If the receiver is signed out or using Free, the same batch should offer Pro/restore and the
individual-file alternative. Sending one file at a time remains free.
```

The owner must provision legitimate reviewer access and test it. Do not fabricate receipts,
invent an existing entitlement or turn on a production test-purchase bypass to satisfy review.
If instructions rely on a browser sender, rehearse that exact browser-to-Android path first;
the browser remains an experimental preview, so the native desktop sender is the default here.

## Internal tester handoff

Use the [test guide and acceptance matrix](../TESTING.md) for track setup, safe fixtures and result records.

1. Owner creates the internal track, supplies the opt-in link and adds the chosen tester accounts.
2. Testers install from that Play track. Preserve files before replacing a debug APK: its signing
   certificate may differ, and uninstalling removes app-local state.
3. Billing testers must also be on Google's **license testers** list. Joining a track alone does
   not make a purchase free. Confirm Google's test payment sheet before any test purchase.
4. Exercise free single-file transfers, then plain and encrypted batches from free desktop to
   both Free and Pro phones. Check upgrade/restore, account mismatch, offline Pro and bulk actions.
5. Record device/Android version, app version/source SHA, expected result, actual result and
   reproducible steps. Submit feedback through the test coordinator's designated private channel;
   omit passwords, purchase tokens, private files and camera recordings unless specifically needed.

Track opt-in and production-access requirements are separate from listing assets. The authoritative
operator checklist remains in the separate website repository's
[PLAY_LAUNCH.md](https://github.com/JoshuaGessner/DataLux-Web/blob/main/PLAY_LAUNCH.md).

## Final review before upload

- Match all screenshots and copy to the exact signed AAB and its Free/Pro configuration.
- Verify the configured US price and the actual checkout sheet before replacing planned wording.
- Check the support inbox, account/deletion links and companion download on a clean device.
- Complete owner-controlled legal, Data Safety, rating and app-access declarations separately.
- Keep upload signing, production billing acceptance and physical camera testing as explicit gates.
- Confirm current Console field/asset requirements; this folder does not submit anything by itself.
