# FileFlow

**Send files between two phones using nothing but light.** One phone shows the file on its screen
as a fast-changing colour pattern; the other reads it with its camera. No Wi-Fi, no Bluetooth, no
cables, no accounts, no internet — FileFlow doesn't even ask for network access.

Every file is checked end to end (SHA-256) before it is saved, so what arrives is exactly what was
sent. With **encrypted mode** on, files are sealed so that only the phone you paired with can open
them.

## How fast

Measured between two phones held about 10 cm apart (Pixel 8 sending, Galaxy S26 Ultra receiving),
with the sending screen filling the receiving camera's view:

| Speed setting | What it sends | Speed |
| --- | --- | --- |
| **Fastest** | 8 colours, 30 frames a second | **≈ 265 KB/s** (2.1 Mbit/s), measured |
| **Fast** (default) | 4 colours, 30 frames a second | ≈ 175 KB/s, expected |
| **Steady** | black and white, 20 frames a second | ≈ 35–45 KB/s, measured |

A 2 MB photo takes about 8 seconds on Fastest and 12 on Fast. Eight colours need a receiving
camera that sees colour sharply; four colours are much easier to tell apart, so **Fast** is the
default and works with more phones. If the receiver says it can read the screen but not its
colours, step the sender down a setting.

Text, documents and other compressible files go faster still: FileFlow compresses them with
Zstandard before sending, so a 1 MB text file may need only a few hundred kilobytes on the link.
Photos and videos are already compressed; a quick check spots them and sends them as they are.
**Settings › Compression** offers **Auto** (the default), **Maximum** (smallest transfer, longer to
prepare) and **Off**. The receiver shows the link's speed and the file's *effective* speed, which
counts the time spent compressing and opening, along with how much time compression saved or cost
compared with sending the file uncompressed. Turn compression off to compare the two.

Speeds depend on both phones — the receiving camera needs to run at 50–60 frames a second for the
faster settings, which most recent phones do. The receiver works out the sender's setting on its
own.

## Using it

**To send:** open FileFlow, tap **Choose a file** (or share a file to FileFlow from any app), and
hold the phone steady with its screen facing the other phone.

**To receive:** tap **Start receiving** and point the camera at the sending screen. Fill the corner
marks with the pattern — closer is faster, as long as the camera can focus. If you move away, the
transfer pauses; aim back and it carries on from where it was. Nothing received is lost.

Received files are saved to **Downloads › FileFlow** and listed in the **Received** tab.

## Sending from a computer

`fileflow-send` turns a laptop, monitor, TV or projector into the sending screen, for any phone
running FileFlow. A bigger screen means bigger cells, so phones can read from further back and
further to the side — one screen, a room full of receivers, each at its own pace.

```sh
cmake --preset desktop-app && cmake --build build/desktop-app
open build/desktop-app/desktop/FileFlow.app                                 # macOS
build/desktop-app/desktop/FileFlow.app/Contents/MacOS/FileFlow report.pdf  # straight to sending
```

On macOS the build makes `FileFlow.app`, with its icon, ready to drag to Applications; files dropped
on its Dock icon are sent. On Windows it makes `FileFlow.exe` with its icon; on Linux
`fileflow-send`, and `cmake --install build/desktop-app` adds it to the applications menu.

With a second display attached — a projector, a TV — **Show the code on** puts the code full screen
there, while this screen becomes the presenter's view: a live copy of the code, what's going, the
speed and **Stop**.

On the home screen, drop files on the window or **Choose files…**; several go as one batch, as on
the phone. **⌘V / Ctrl+V** sends the text on the clipboard — a link, an address, a note — as
`Clipboard.txt`. Speed and compression are chosen there too and remembered, and your last few sends
are listed to **Send again**.

Sending goes full screen. The name of what's going, how long one pass over it takes, the speed and
**Stop** float over the code for a few seconds and then fade, so the screen shows only the code;
move the mouse or press a key to bring them back. **Esc** stops. Keys: **1 2 3** change speed,
**F** toggles full screen. Options: `--speed steady|fast|fastest`, `--compression off|auto|max`,
`--windowed`.

It needs CMake 3.25 and a C++20 compiler; SDL3, the Inter typeface and stb_truetype are fetched,
pinned, and built in, so the result is a single program with nothing to install. Tested on macOS,
and it compiles on Linux (GCC 15); Windows is next to be checked. Encrypted sending from a
computer isn't there yet.

## Encrypted mode

Turn on **Encrypted** on both phones (the switch is on the Send and Receive tabs). The first time,
FileFlow asks you to **pair**: hold the two phones face to face, screens together. Each shows a QR
code and reads the other's with its front camera; both then show the same six-digit code. If the
codes match, you're paired.

- Each file is encrypted with **AES-256-GCM** under a key that only the two paired phones hold.
  The file's name travels inside the encryption too.
- The key comes from a fresh **ECDH (P-256)** exchange made just for that pairing and is never
  stored: it lasts until FileFlow closes, or 30 minutes.
- In encrypted mode the receiver refuses anything that isn't sealed to its pairing, including
  unencrypted files, so encryption can't be quietly switched off by the sender.

### Teams

A team is a group that sends encrypted files to each other: one sending screen, read by every
member's camera at once, opens on all their phones and nobody else's. Make one in
**Settings › Teams** — you lead it — then open it and tap **Invite someone** while the new member
taps **Join a team**, and hold the two phones face to face. They pair as above, and the leader's
screen then shows the team's key **sealed to that pairing**, so a camera across the room learns
nothing from it. The new member's phone answers with their name, which joins the leader's list.

- Each team has one AES-256 key, kept on the phone and wrapped by the Android Keystore.
- Set the name others see under **Teams › Your name**.
- The leader sees everyone in the team, and can rename, remove people and delete it; a member sees
  the team as it was when they joined, and can leave.
- To send to a team, tap **Encrypted** and choose it. Receiving needs nothing: a file for any team
  you're in opens by itself.
- Holding the key is membership, and a phone can't be made to forget a key. So removing someone
  gives the team a new key, and the leader invites everyone else again.

Without encrypted mode, a FileFlow transfer is like a QR code: anyone whose camera can see the
sending screen could read it. See [SECURITY.md](SECURITY.md) for the details.

## Requirements

- Android 13 or newer, on a 64-bit ARM phone (practically every phone from the last several years).
- A rear camera for receiving, and a front camera for pairing.

## Installing

FileFlow is built from source for now. Open the project in a recent Android Studio, let it sync,
and run the **app** configuration on your phone. (Command line: with Gradle 9.8 and the Android
SDK installed, `gradle :app:assembleRelease`.)

## License

FileFlow is free software under the **GNU Affero General Public License v3.0** — see
[LICENSE](LICENSE). You may use, study, change and share it; if you distribute it or offer it as a
service, modified or not, you must make your complete source available under the same license.

**Commercial licensing:** to build FileFlow's technology into a closed-source product, contact the
copyright holder for a commercial license.

"FileFlow" and the FileFlow logo identify this project; the license does not grant permission to
use them for other products.

## Contributing

Bug reports and pull requests are welcome. By submitting a contribution you agree that it may be
distributed under the AGPL-3.0 and that the copyright holder may also license it commercially, so
that the dual licensing above stays possible.

Copyright © 2026 Josh Gessner.
