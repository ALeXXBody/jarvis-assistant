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
# The three small part images are published so the browser page can re-flash
# a board *without* erasing NVS (Wi-Fi credentials + brain URL survive an
# update). They are written at their own offsets, never over the NVS region:
#   bootloader.bin         bootloader      -> 0x2000
#   partitions.bin         partition table -> 0x9000
#   ota_data_initial.bin   OTA slot select -> 0x10000  (resets boot to ota_0)
#
set -euo pipefail

cd "$(dirname "$0")/.."                 # -> firmware/
VERSION="${1:?usage: tools/release.sh vX.Y.Z [notes]}"
VER_NUM="${VERSION#v}"
NOTES="${2:-JARVIS panel firmware $VERSION}"
REPO="${JARVIS_REPO:-ALeXXBody/jarvis-assistant}"

echo ">> building firmware $VER_NUM"
# PLATFORMIO parses PLATFORMIO_BUILD_FLAGS with shlex, which strips plain
# quotes — the inner \" must stay literal so the macro is a *string*, not a
# number. (${x:-} keeps any flags you already had.)
#
# The version the panel actually reports (OTA check, setup sheet, bottom bar)
# comes from the built image's app descriptor. IDF picks that up from
# firmware/version.txt (kept out of the repo -.gitignore-), so a release
# image carries exactly "$VER_NUM"; dev builds fall back to git describe and
# release.sh removes the file afterwards so they stay truthful.
printf '%s\n' "$VER_NUM" > version.txt
PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} -D JARVIS_FW_VERSION=\\\"$VER_NUM\\\"" \
    pio run -e jarvis-panel
rm -f version.txt

BUILD=.pio/build/jarvis-panel
mkdir -p dist
cp "$BUILD/firmware.bin"         dist/firmware.bin
cp "$BUILD/firmware.factory.bin" dist/firmware.factory.bin
cp "$BUILD/bootloader.bin"       dist/bootloader.bin
cp "$BUILD/partitions.bin"       dist/partitions.bin
cp "$BUILD/ota_data_initial.bin" dist/ota_data_initial.bin
printf '%s\n' "$VER_NUM" > dist/version.txt

echo ">> creating release $VERSION on $REPO"
gh release create "$VERSION" \
    dist/firmware.bin dist/firmware.factory.bin dist/version.txt \
    dist/bootloader.bin dist/partitions.bin dist/ota_data_initial.bin \
    --repo "$REPO" \
    --title "Panel firmware $VERSION" \
    --notes "$NOTES"

echo
echo ">> published. The panel's OTA source is:"
echo "   https://github.com/$REPO/releases/latest/download/"
echo
echo ">> To test the browser installer from this release:"
echo "   cd flash && ./get-latest.sh && python3 -m http.server 8000"
echo "   (open http://localhost:8000 in Chrome/Edge)"
