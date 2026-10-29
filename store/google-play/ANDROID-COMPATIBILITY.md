# Android native compatibility evidence

> **Status:** Static checks passed; 16 KB runtime test awaiting emulator launch
> **Owner:** Release engineering
> **Last reviewed:** 2026-10-29

## Artifact and completed checks

The local debug candidate is DataLux `dev.datalux` 0.4.1 / code 5, functional source
`edccb3c44c72d81cf67a1d9cee019c26801db1ba`. Subsequent preparation commits changed documentation
and store materials. This is not the signed production candidate or proof of Play acceptance.

| Evidence | Result |
| --- | --- |
| Debug APK SHA-256 | `26e585f6adc4fc2817508339d69b599f28352a72a6c9dee9701e6dd4d99ed6e5` |
| ARM64 `libfileflow.so` SHA-256 | `ae6e1d36311784af0b0e5ce855f842b9826d4f574a6969e2acbdaf93ade31e0d` |
| Native ELF LOAD and RELRO checks | PASS using the existing release checker, including the safe dedicated-RELRO-LOAD rule |
| Native APK packaging | One uncompressed ARM64 library; ZIP data offset 15,958,016 is divisible by 16,384 |
| Source review | No actionable kernel-page-size assumption found in core/JNI or the bundled Zstd allocation paths; ordinary protocol limits and image dimensions are distinct |
| Physical phone | SM-S948U1 / Android 17: page size 4096; debug launch/navigation checked, not 16 KB runtime evidence |
| Test harness preparation | Java compilation, D8 conversion and Python syntax/CLI checks passed |

NDK r29 and the existing release checker already cover static 16 KB packaging. The source review
does not replace runtime tests. Follow [Google's 16 KB guidance](https://developer.android.com/guide/practices/page-sizes).

## Isolated runtime environment

Google's official `system-images;android-37.0;google_apis_ps16k;arm64-v8a` image was installed
under ignored `build/artifacts/android-16k/sdk`. A dedicated `DataLux_16K_API37` AVD and its
user/configuration files are under the same ignored test directory. Emulator 37.2.12 is also
installed there; the launcher supplies the API 37 minimum of 4 GB RAM and software graphics. Existing SDK images, user
AVDs, phone, watch and production services were not changed.

The initial agent-hosted emulator could not boot: hardware acceleration was unavailable and
QEMU reported denied memory-protection operations. The old global emulator was also older than
API 37 support. That attempt was stopped; the owner was asked to restart the updated isolated
emulator from a normal Terminal. Runtime results remain pending. Launch the prepared
device from a normal Mac Terminal, leaving that Terminal open:

```sh
cd /Users/josh/dev/fileflow
sh scripts/start_android_16k_emulator.sh
```

The launcher uses port 5580 and this task's isolated AVD/emulator. It defaults to the normal macOS
SDK path for platform tools and does not download components or accept terms. Existing SDK
emulator binaries are left unchanged.

After Android boots, run the native smoke check from the repository root (adjust SDK/JDK paths
on another computer):

```sh
python3 scripts/android_runtime_smoke.py \
  --serial emulator-5580 \
  --apk app/build/outputs/apk/debug/app-debug.apk \
  --sdk /Users/josh/Library/Android/sdk \
  --java-home /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
  --output build/artifacts/android-16k/smoke
```

The runner refuses physical-device serials, another AVD name, incomplete boot and a kernel page
size other than **16384**. It temporarily disables 16 KB compatibility fallback on this dedicated
Android 17 AVD (fatal linker mode), verifies that setting, and restores the prior properties
afterward. A failed run replaces any previous PASS report with FAIL. It loads the actual APK's DEX/JNI library, uses synthetic bytes for
compression round trips around 4 KB/16 KB boundaries, verifies output-length/input-slice rejection,
generates frames in all three colour modes, checks payload size/hash and buffer bounds, opens a
receiver on a blank synthetic frame, and installs/launches the APK on this dedicated emulator.
It removes only its generated remote test directory and stops DataLux there afterward.

The report includes APK/native hashes, kernel page size and actual outcomes. A successful run
does not prove optical recovery, real camera/display behavior, Play purchase/restore/refund flows,
production license trust, or the final Play-signed bundle. Repeat against that exact candidate
after the deferred signer work, and keep physical optical acceptance separate.

See [acceptance matrix](TESTING.md), [upload readiness](UPLOAD-READINESS.md), and
[deferred NUC work](NUC-HANDOFF.md).
