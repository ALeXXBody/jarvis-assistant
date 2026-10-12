#!/usr/bin/env bash
#
# Build the JARVIS panel firmware and publish it as a GitHub release, so that
# (a) a first-time user can download a flashable image, and (b) already-flashed
# panels can pull the update over the air.
#
# Usage:
#   tools/release.sh v0.2.0 ["release notes"]
#
# Env:
#   JARVIS_REPO            owner/repo to publish to  (default: ALeXXBody/jarvis-assistant)
#   PLATFORMIO_BUILD_FLAGS any flags you already use are kept; the version is appended
#
# Published assets — the OTA updater fetches the last two by exact name:
#   firmware.factory.bin   combined image  -> flash at 0x0 on a fresh board
#   firmware.bin           app image       -> OTA payload
#   version.txt            plain version   -> OTA version check
#
set -euo pipefail

cd "$(dirname "$0")/.."                 # -> firmware/
VERSION="${1:?usage: tools/release.sh vX.Y.Z [notes]}"
VER_NUM="${VERSION#v}"
NOTES="${2:-JARVIS panel firmware $VERSION}"
REPO="${JARVIS_REPO:-ALeXXBody/jarvis-assistant}"

echo ">> building firmware $VER_NUM"
PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} -D JARVIS_FW_VERSION=\"$VER_NUM\"" \
    pio run -e jarvis-panel

BUILD=.pio/build/jarvis-panel
mkdir -p dist
cp "$BUILD/firmware.bin"         dist/firmware.bin
cp "$BUILD/firmware.factory.bin" dist/firmware.factory.bin
printf '%s\n' "$VER_NUM" > dist/version.txt

echo ">> creating release $VERSION on $REPO"
gh release create "$VERSION" \
    dist/firmware.bin dist/firmware.factory.bin dist/version.txt \
    --repo "$REPO" \
    --title "Panel firmware $VERSION" \
    --notes "$NOTES"

echo
echo ">> published. The panel's OTA source is:"
echo "   https://github.com/$REPO/releases/latest/download/"
