# DataLux Android tester quick start

> **Draft:** Share after a reviewed build is available in the intended Play testing track and
> the coordinator fills in its invitation and feedback details. This is not a release announcement.

## Join and install

1. Give the coordinator the Google account email you use in Google Play. Wait until they confirm
   that account has been added to the test and send you its opt-in link. The app may not appear
   in Play search; use the link supplied by the coordinator.
2. Open the link while signed into the invited Google account, join the test, then follow the
   link to install DataLux from Google Play. If you have multiple Google accounts, make sure
   the Play Store uses the invited account. Availability can take time after rollout.
3. If asked to replace an older debug or sideloaded build, **save any files you want to keep
   before uninstalling it**. Uninstalling can remove app-local data. Ask the coordinator if you
   are unsure; an old FileFlow package may be installed separately.
4. Open the app and report whether it starts. Keep the Play test installed and opted in while
   participating. For a closed test that counts toward production eligibility, stay opted in for
   the period the coordinator requests; opting out can interrupt the continuous period.

## Try a first transfer

You need an Android phone with a working camera and a second compatible device with a screen.
Begin with a small, disposable text file that contains no private information. Ask the coordinator
which companion build and version to use; experimental desktop and browser builds are separate.

1. Open DataLux on both devices. Choose the text file on the sending device and begin sending.
2. Open Receive on the Android phone and allow camera access when prompted. Point the camera at
   the sender's displayed code. Keep the code in view and adjust screen brightness, distance or
   angle if needed. Record settings if you encounter a problem.
3. Once the app says the transfer is complete, open the received file and compare its contents
   with the original. Tell the coordinator whether it matched, failed, or got stuck. If you can
   calculate SHA-256 hashes on both devices, include whether the hashes match.
4. If possible, try the reverse direction with another small test file, then note anything that
   was confusing or unexpected. Do not assume a completion message alone proves file integrity.

For an interrupted or failed transfer, record what was displayed and the steps before trying
again. Use only synthetic files and test accounts, and keep camera surroundings free of private
information. Debug builds may save camera frames during testing, and diagnostic logs can include
file names. Don't send photos of real documents or other private content with a bug report.

## Updates and purchases

Once the coordinator publishes an update to your Play track, Google Play can update the app
automatically. You can also open the app's Play listing and tap **Update** when offered. Timing
depends on Google Play and device settings. Report the installed version with each issue.

**Do not test purchasing unless the coordinator specifically asks you to.** Test-track access
alone does not prevent real charges. For an authorized purchase test, the coordinator must also
add the purchasing Google account as a license tester and explain the approved test environment.
Confirm the correct account and Google's test-purchase notice in the purchase dialog; stop if
either is missing. Do not share payment details, purchase tokens, passwords or verification codes.

## Send feedback

Use the private channel supplied by the coordinator. Send one report per issue, with:

```text
What happened and what did you expect?
Steps to reproduce:
How often does it happen?
DataLux version / install source / test track:
Phone model and Android version:
Other device model, OS and DataLux version:
Transfer direction, mode, approximate file size, distance and lighting:
Exact error message (remove personal information):
Did the received test file match the original?
```

Review all screenshots and diagnostic exports, including camera images and file names, for personal
information before sharing them, and use only the coordinator's private channel. Never include
passwords, email verification links, account tokens, purchase receipts, pairing keys, or real
personal files. Ask the coordinator how to report an issue if an export cannot be safely redacted.

**Coordinator: fill in before sharing**

- Track and eligible countries: _pending_
- Play opt-in link: _pending; copy from the intended track after it is available_
- Expected app version and companion build: _pending_
- Private feedback channel: _pending_
- What to test and any known limitations: _pending_

Google's references: [testing tracks and opt-in](https://support.google.com/googleplay/android-developer/answer/9845334),
[testing Play billing](https://developer.android.com/google/play/billing/test), and
[closed-testing eligibility](https://support.google.com/googleplay/android-developer/answer/14151465).
