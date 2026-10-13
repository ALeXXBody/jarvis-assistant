# Browser installer for the JARVIS panel

Flash the GUITION JC8012P4A1C (ESP32-P4) panel from a web browser — no
toolchain required. This folder is an
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) page (the same
installer technology ESPHome uses) wrapped around the merged image.

## Quick install (works today even with a private repo)

```bash
cd flash
./get-latest.sh                    # pulls firmware.factory.bin from the latest release
python3 -m http.server 8000        # serve this folder
# open http://localhost:8000 in Chrome / Edge / Opera, click Install
```

Web Serial needs a Chromium-based desktop browser (Chrome, Edge, Opera);
Firefox and Safari are not supported. Serving from `localhost` over plain HTTP
is accepted (it counts as a secure context).

The page needs `manifest.json` and `firmware.factory.bin` as siblings:
**the firmware must be served from the same origin as the page** — GitHub
release assets are not CORS-accessible, so a manifest pointing at
`releases/latest/download/…` will not work from another website.

## One-click URL, once the repository is public

`.github/workflows/flasher.yml` publishes this folder (with the image baked in)
to **GitHub Pages** on every release. Enable Pages and the panel becomes
installable from a plain URL with a single click — no clone, no script.

## Files

| File | Purpose |
|---|---|
| `index.html` | the installer page (ESP Web Tools button + manual-flash fallback) |
| `console.html` | live serial log viewer in the browser (Web Serial over USB, zero-install) |
| `manifest.json` | ESP Web Tools manifest: `chipFamily ESP32-P4`, the merged image at **offset 0** (fresh install, erases everything) |
| `manifest-update.json` | ESP Web Tools manifest that re-flashes only bootloader / partition table / OTA slot selector / app, so the settings (NVS) region — Wi-Fi credentials and brain URL — is preserved |
| `get-latest.sh` | fetches `firmware.factory.bin` plus the update parts from the latest release |
| `firmware.factory.bin` | (generated, not committed) the merged image, from `flash/get-latest.sh` or `pio run` |

## Keeping settings across an update

Wi-Fi credentials and the brain URL live in the panel's NVS partition
(`0xa000`–`0x10000`). Two update paths preserve it:

- **On-device OTA** (the panel's own auto-update, and its **UPDATE** button on
  the setup page) writes only the app slot — NVS is never touched.
- **Browser *Update* button** on `index.html` uses `manifest-update.json`,
  which writes only `0x2000` (bootloader), `0x9000` (partition table),
  `0x10000` (OTA slot selector) and `0x20000` (app). None of those overlap
  NVS, so settings survive.

The **Install** button (merged image at offset 0) is a full-chip flash: it
erases NVS, so use it only for a first install or to wipe the board.
