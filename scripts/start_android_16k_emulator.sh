#!/bin/sh
# Launch only the isolated DataLux test AVD prepared under the ignored build directory.
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root="$task_root/build/artifacts/android-16k"
sdk_root=${ANDROID_HOME:-"$HOME/Library/Android/sdk"}
emulator="$test_root/sdk/emulator/emulator"
image="$test_root/sdk/system-images/android-37.0/google_apis_ps16k/arm64-v8a"
if [ ! -f "$test_root/avd/DataLux_16K_API37.ini" ] || [ ! -f "$image/system.img" ] || [ ! -x "$emulator" ]; then
    echo 'The isolated DataLux_16K_API37 AVD/image has not been prepared on this computer.' >&2
    exit 1
fi
case "$(sed -n 's/^target=//p' "$test_root/avd/DataLux_16K_API37.ini")" in
    android-37|android-37.0) ;;
    *) echo 'The isolated AVD must identify API 37 correctly; recreate it with current SDK tools.' >&2; exit 1 ;;
esac
export ANDROID_AVD_HOME="$test_root/avd"
export ANDROID_USER_HOME="$test_root/user"
export ANDROID_SDK_ROOT="$sdk_root"
exec "$emulator" -avd DataLux_16K_API37 -sysdir "$image" \
    -no-snapshot -no-boot-anim -no-audio -gpu software -port 5580 -memory 4096
