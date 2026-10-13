#!/usr/bin/env bash
#
# Drop the panel image next to index.html so the ESP Web Tools page can serve
# it. The .bin must be a sibling of manifest.json (see flash/index.html).
#
#   Public repo: plain curl works.
#   Private repo: needs the 'gh' CLI, authenticated as someone with read access
#                 (the anonymous /releases/latest/download/ URL 404s while the
#                 repository is private).
#
#   JARVIS_REPO  optional override of the owner/repo that holds the release.
#
set -euo pipefail

cd "$(dirname "$0")"
REPO="${JARVIS_REPO:-ALeXXBody/jarvis-assistant}"

if command -v gh >/dev/null 2>&1; then
    echo ">> latest release:"
    gh release list --repo "$REPO" --limit 1 || true
    echo ">> downloading firmware.factory.bin from $REPO latest release"
    gh release download --repo "$REPO" --pattern 'firmware.factory.bin' --clobber
    echo ">> downloading the update parts (bootloader / partitions / otadata / app)"
    gh release download --repo "$REPO" \
        --pattern 'bootloader.bin' --pattern 'partitions.bin' \
        --pattern 'ota_data_initial.bin' --pattern 'firmware.bin' --clobber
else
    echo ">>> no 'gh' CLI found; trying a plain anonymous download (works once"
    echo ">>> the repository is public)"
    curl -fL --retry 3 -o firmware.factory.bin \
        "https://github.com/$REPO/releases/latest/download/firmware.factory.bin"
    for asset in bootloader.bin partitions.bin ota_data_initial.bin firmware.bin; do
        curl -fL --retry 3 -o "$asset" \
            "https://github.com/$REPO/releases/latest/download/$asset"
    done
fi

echo
echo ">> fetched files (compare sizes with the release page):"
ls -l firmware.factory.bin bootloader.bin partitions.bin ota_data_initial.bin firmware.bin
echo "   if a file is missing or truncated, re-run this script."
curl -fsSL --retry 3 -o version.txt --clobber \
    "https://github.com/$REPO/releases/latest/download/version.txt" 2>/dev/null \
    || gh release download --repo "$REPO" --pattern 'version.txt' --clobber
echo ">> installed firmware release: $(cat version.txt 2>/dev/null || echo '?')"
echo ">> done. Serve this folder, then open the page:"
echo "   python3 -m http.server 8000"
echo "   -> http://localhost:8000"
