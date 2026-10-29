#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build_macos_dmg.sh --app DataLux.app --output DataLux-vVERSION-macos-ARCH.dmg \
  --version VERSION --arch arm64|x86_64|universal2 --source-revision 40_HEX_SHA \
  [--license LICENSE] [--readme README.md] [--report REPORT.json] \
  [--background-preview BACKGROUND.png]
EOF
}

APP=""
OUTPUT=""
VERSION=""
ARCH=""
SOURCE_REVISION=""
LICENSE_PATH=""
README_PATH=""
REPORT_PATH=""
BACKGROUND_PREVIEW=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP="${2:-}"; shift 2 ;;
    --output) OUTPUT="${2:-}"; shift 2 ;;
    --version) VERSION="${2:-}"; shift 2 ;;
    --arch) ARCH="${2:-}"; shift 2 ;;
    --source-revision) SOURCE_REVISION="${2:-}"; shift 2 ;;
    --license) LICENSE_PATH="${2:-}"; shift 2 ;;
    --readme) README_PATH="${2:-}"; shift 2 ;;
    --report) REPORT_PATH="${2:-}"; shift 2 ;;
    --background-preview) BACKGROUND_PREVIEW="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 64 ;;
  esac
done

if [[ -z "$APP" || -z "$OUTPUT" || -z "$VERSION" || -z "$ARCH" || -z "$SOURCE_REVISION" ]]; then
  usage >&2
  exit 64
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "macOS is required to build and verify a DMG" >&2
  exit 1
fi
if [[ ! "$VERSION" =~ ^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$ ]]; then
  echo "version must be a SemVer with an optional v prefix" >&2
  exit 64
fi
if [[ ! "$SOURCE_REVISION" =~ ^[0-9a-f]{40}$ ]]; then
  echo "source revision must be a lowercase 40-character Git SHA" >&2
  exit 64
fi
case "$ARCH" in arm64|x86_64|universal2) ;; *) echo "unsupported architecture: $ARCH" >&2; exit 64 ;; esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPOSITORY_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
PACKAGING_DIR="$REPOSITORY_ROOT/desktop/macos/packaging"
APP="$(cd "$(dirname "$APP")" && pwd -P)/$(basename "$APP")"
APP_NAME="$(basename "$APP")"
if [[ ! -d "$APP" || ( "$APP_NAME" != "FileFlow.app" && "$APP_NAME" != "DataLux.app" ) ]]; then
  echo "--app must name an existing DataLux.app or historical FileFlow.app bundle" >&2
  exit 1
fi
PRODUCT_NAME="${APP_NAME%.app}"
OUTPUT_PARENT="$(dirname "$OUTPUT")"
mkdir -p "$OUTPUT_PARENT"
OUTPUT="$(cd "$OUTPUT_PARENT" && pwd -P)/$(basename "$OUTPUT")"
if [[ "${OUTPUT##*.}" != dmg ]]; then
  echo "--output must end in .dmg" >&2
  exit 64
fi
if [[ -e "$OUTPUT" ]]; then
  echo "refusing to overwrite existing output: $OUTPUT" >&2
  exit 1
fi

APP_BINARY="$APP/Contents/MacOS/$PRODUCT_NAME"
APP_ICON="$APP/Contents/Resources/FileFlow.icns"
if [[ ! -x "$APP_BINARY" || ! -f "$APP_ICON" || ! -f "$APP/Contents/Info.plist" ]]; then
  echo "$APP_NAME is missing its executable, icon, or Info.plist" >&2
  exit 1
fi
NORMALIZED_VERSION="${VERSION#v}"
APP_BASE_VERSION="${NORMALIZED_VERSION%%-*}"
APP_VERSION="$(plutil -extract CFBundleShortVersionString raw -o - "$APP/Contents/Info.plist")"
if [[ "$APP_VERSION" != "$APP_BASE_VERSION" ]]; then
  echo "app version $APP_VERSION does not match requested core version $APP_BASE_VERSION" >&2
  exit 1
fi

APP_ARCHES="$(lipo -archs "$APP_BINARY")"
case "$ARCH" in
  arm64|x86_64)
    if [[ "$APP_ARCHES" != "$ARCH" ]]; then
      echo "app architectures '$APP_ARCHES' do not match $ARCH" >&2
      exit 1
    fi
    ;;
  universal2)
    if [[ " $APP_ARCHES " != *" arm64 "* || " $APP_ARCHES " != *" x86_64 "* ]]; then
      echo "universal2 requires arm64 and x86_64; found '$APP_ARCHES'" >&2
      exit 1
    fi
    ;;
esac
codesign --verify --deep --strict --verbose=2 "$APP"

if [[ -z "$LICENSE_PATH" && -f "$(dirname "$APP")/LICENSE" ]]; then LICENSE_PATH="$(dirname "$APP")/LICENSE"; fi
if [[ -z "$README_PATH" && -f "$(dirname "$APP")/README.md" ]]; then README_PATH="$(dirname "$APP")/README.md"; fi
for DOCUMENT in "$LICENSE_PATH" "$README_PATH"; do
  if [[ -n "$DOCUMENT" && ! -f "$DOCUMENT" ]]; then
    echo "metadata document does not exist: $DOCUMENT" >&2
    exit 1
  fi
done

WORK_DIR="$(mktemp -d "${TMPDIR:-/private/tmp}/fileflow-dmg.XXXXXX")"
MOUNT_DIR="$WORK_DIR/mount"
DEVICE=""
SUCCEEDED=false
cleanup() {
  local status=$?
  if [[ -n "$DEVICE" ]]; then hdiutil detach "$DEVICE" >/dev/null 2>&1 || true; fi
  if [[ "$SUCCEEDED" != true && -f "$OUTPUT" ]]; then rm -f "$OUTPUT"; fi
  rm -rf "$WORK_DIR"
  exit "$status"
}
trap cleanup EXIT

METADATA_DIR="$WORK_DIR/metadata"
BACKGROUND="$WORK_DIR/background.png"
VENV="$WORK_DIR/venv"
mkdir -p "$METADATA_DIR" "$MOUNT_DIR"
printf '%s\n' "$SOURCE_REVISION" > "$METADATA_DIR/SOURCE_REVISION.txt"
if [[ -n "$LICENSE_PATH" ]]; then cp "$LICENSE_PATH" "$METADATA_DIR/LICENSE"; fi
if [[ -n "$README_PATH" ]]; then cp "$README_PATH" "$METADATA_DIR/README.md"; fi

swift "$PACKAGING_DIR/render_background.swift" "$BACKGROUND" "$NORMALIZED_VERSION" "$ARCH" "$PRODUCT_NAME"
if [[ -n "$BACKGROUND_PREVIEW" ]]; then
  mkdir -p "$(dirname "$BACKGROUND_PREVIEW")"
  cp "$BACKGROUND" "$BACKGROUND_PREVIEW"
fi

python3 -m venv "$VENV"
PIP_DISABLE_PIP_VERSION_CHECK=1 "$VENV/bin/python" -m pip install \
  --only-binary=:all: --require-hashes --no-cache-dir \
  -r "$PACKAGING_DIR/requirements-dmg.txt"

ORIGINAL_BUNDLE_DIGEST="$(python3 "$PACKAGING_DIR/package_tools.py" bundle-digest "$APP")"
"$VENV/bin/dmgbuild" \
  -s "$PACKAGING_DIR/dmg_settings.py" \
  -D "app=$APP" \
  -D "metadata=$METADATA_DIR" \
  -D "background=$BACKGROUND" \
  -D "volume_icon=$APP_ICON" \
  "$PRODUCT_NAME" "$OUTPUT"

hdiutil verify "$OUTPUT"
ATTACH_OUTPUT="$(hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_DIR" "$OUTPUT")"
DEVICE="$(awk '/^\/dev\// { print $1; exit }' <<< "$ATTACH_OUTPUT")"
if [[ -z "$DEVICE" ]]; then
  echo "could not identify the mounted DMG device" >&2
  exit 1
fi
if [[ ! -d "$MOUNT_DIR/$APP_NAME" || ! -L "$MOUNT_DIR/Applications" || \
      "$(readlink "$MOUNT_DIR/Applications")" != "/Applications" ]]; then
  echo "DMG is missing $APP_NAME or the /Applications symlink" >&2
  exit 1
fi
cmp "$METADATA_DIR/SOURCE_REVISION.txt" "$MOUNT_DIR/.fileflow/SOURCE_REVISION.txt"
MOUNTED_BUNDLE_DIGEST="$(python3 "$PACKAGING_DIR/package_tools.py" bundle-digest "$MOUNT_DIR/$APP_NAME")"
if [[ "$MOUNTED_BUNDLE_DIGEST" != "$ORIGINAL_BUNDLE_DIGEST" ]]; then
  echo "$APP_NAME changed while packaging" >&2
  exit 1
fi
if [[ "$(lipo -archs "$MOUNT_DIR/$APP_NAME/Contents/MacOS/$PRODUCT_NAME")" != "$APP_ARCHES" ]]; then
  echo "mounted application architectures changed" >&2
  exit 1
fi
codesign --verify --deep --strict --verbose=2 "$MOUNT_DIR/$APP_NAME"
hdiutil detach "$DEVICE" >/dev/null
DEVICE=""
hdiutil verify "$OUTPUT"

if [[ -n "$REPORT_PATH" ]]; then
  "$VENV/bin/python" "$PACKAGING_DIR/package_tools.py" write-report \
    --dmg "$OUTPUT" \
    --output "$REPORT_PATH" \
    --version "$NORMALIZED_VERSION" \
    --arch "$ARCH" \
    --source-revision "$SOURCE_REVISION" \
    --app-tree-sha256 "$ORIGINAL_BUNDLE_DIGEST"
fi
SUCCEEDED=true
echo "Built and verified $OUTPUT"
