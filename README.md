# JARVIS voice satellite

A fully local, private voice assistant built on the **GUITION JC8012P4A1C_I_W_Y**
10.1″ touch panel (ESP32-P4). You tap the screen (or press a key on the serial
console), speak, and the panel streams your voice to a local "brain", then
speaks the reply while showing a live arc-reactor status display and transcript.

* **No cloud.** STT (faster-whisper), the LLM (local Ollama `gpt-oss:20b`) and
  TTS (piper) all run on a box on your LAN.
* **Custom firmware.** Not the vendor demo — an ESP-IDF app written for this
  exact board, with a hand-rolled direct-framebuffer UI (no LVGL).
* **Small and clean.** Builds in well under a minute, no compiler warnings;
  Flash ≈ 24 %, static RAM ≈ 4.8 % of the app slots.
* **Self-updating.** The panel pulls new firmware from GitHub releases over the
  air (with bootloader rollback safety) — no cable needed after the first flash.

> Status: **firmware builds clean; not yet validated on physical hardware.**
> Display / touch / audio drivers are written against verified driver APIs and
> are the next thing to test on a real panel. See [`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md).

---

## Table of contents

- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Repository layout](#repository-layout)
- [The brain (bridge)](#the-brain-bridge)
- [Firmware](#firmware)
- [Building](#building)
- [Flashing the panel](#flashing-the-panel)
- [Updating over the air (OTA)](#updating-over-the-air-ota)
- [First boot & verification](#first-boot--verification)
- [Configuration](#configuration)
- [Documentation](#documentation)

---

## How it works

```
        ┌────────────── ESP32-P4 panel (firmware/) ────────────┐
 mic ─▶ │ jarvis_audio   capture task (16 kHz mono S16LE)      │
        │      │                                               │
        │      ▼  binary WS frames                             │
        │ jarvis_ws ──────── ws://brain:8765/ws ──────────────▶ │
        │      │  ◀──── text JSON events + binary TTS PCM ───── │
        │      ├─▶ jarvis_state   (mutex-protected snapshot)   │
        │      ├─▶ jarvis_audio   playback task ──▶ speaker    │
        │      ├─▶ jarvis_ui      direct RGB565 framebuffer ─▶ LCD
        │      └─  jarvis_turn    turn / approval state machine│
        │          jarvis_wake    touch tap + serial console    │
        └──────────────────────────────────────────────────────┘
                                  │
                    ┌─────────────┴──────────────┐
                    │  assistant host (bridge/)   │
                    │  STT → Hermes Agent → TTS   │
                    │  local Ollama (gpt-oss:20b) │
                    └─────────────────────────────┘
```

The panel is a thin **satellite**: it owns the hardware (screen, mic, speaker,
touch) and speaks a small JSON + binary WebSocket protocol to the bridge, which
owns all the intelligence. See [`firmware/docs/PROTOCOL.md`](firmware/docs/PROTOCOL.md).

---

## Hardware

| | |
|---|---|
| Module | GUITION **JC8012P4A1C_I_W_Y** |
| SoC | **ESP32-P4** dual-core RISC-V, 360–400 MHz |
| Memory | 768 KB HP L2MEM + **32 MB stacked PSRAM** |
| Flash | **16 MB** NOR (W25Q128) |
| Wireless | **ESP32-C6-MINI-1** (Wi-Fi 6 + BLE 5) via **ESP-Hosted over SDIO** — the P4 has no radio |
| Display | 10.1″ IPS, **800 × 1280**, **JD9365** driver, 2-lane **MIPI-DSI** |
| Touch | **GSL3680** capacitive, I²C `0x40` |
| Audio | **ES8311** mono codec (I²S + I²C) + onboard mic + speaker amp |
| Storage | microSD (TF), 4-bit SDMMC |
| Other | RTC (RX8025T), **WS2812** RGB LED, 3.7 V Li-ion connector, power button |
| Power | 5 V DC, ~700 mA — **use a ≥ 2 A supply** |

### Pin map (verified against the vendor BSP / community)

```
Display   JD9365 MIPI-DSI 2-lane, reset GPIO27, backlight GPIO23 (LEDC, active-high)
Touch     GSL3680  SDA GPIO7  SCL GPIO8  RESET GPIO22  INT GPIO21   (I2C 0x40)
Audio     ES8311   MCLK GPIO13  BCLK GPIO12  LRCLK GPIO10  DOUT GPIO9  DIN GPIO11  PA_CTRL GPIO20
Wi-Fi/C6  SDIO     CLK18 CMD19 D0=14 D1=15 D2=16 D3=17  C6 reset GPIO54
microSD   CLK43 CMD44 D0=39 D1=40 D2=41 D3=42
Battery   ADC GPIO52 (divider ×1.68)
```

Full research, spec sheet and options: [`PANEL_RESEARCH.md`](PANEL_RESEARCH.md).

---

## Repository layout

```
.
├── firmware/               ESP-IDF application for the panel
│   ├── src/                application: jarvis_* modules
│   ├── components/
│   │   └── bsp_guition/    board support (display, touch, audio, led, battery)
│   │                       + embedded GSL3680 touch firmware blob
│   ├── boards/             custom pioarduino board definition
│   ├── docs/               BRINGUP.md, PROTOCOL.md
│   ├── tools/              release.sh — build + publish a firmware release
│   ├── platformio.ini      build config (env: jarvis-panel)
│   ├── sdkconfig.defaults  P4 / flash / PSRAM / partition defaults
│   └── partitions.csv      16 MB flash layout
├── flash/                  browser installer (ESP Web Tools) + manual fallback
├── bridge/                 the brain: voice pipeline (STT/LLM/TTS) + WS server
│   ├── server.py           WebSocket server the panel talks to
│   ├── llmproxy.py         LLM transport shim (forces reasoning off)
│   ├── server.yaml         live config (env-var names + paths, no secrets)
│   └── server.example.yaml sanitised example config
├── PANEL_RESEARCH.md       hardware research + architecture options
├── BRIDGE_NOTES.md         brain deployment notes + latency tuning
└── vendor/                 vendor-archive fetch script + manifest
```

---

## The brain (bridge)

The intelligence lives on a separate host (`192.0.2.10`). It is an
adapter of [`eadmin2/jarvis_ai`](https://github.com/eadmin2/jarvis_ai): a
WebSocket voice server in front of **Hermes Agent**, with local STT and TTS.

| Service | Port | Purpose |
|---|---|---|
| `ollama` | 127.0.0.1:11434 | local LLM (`gpt-oss:20b`, 64K ctx) |
| `llmproxy` | 127.0.0.1:11440 | forces reasoning/think off (latency) |
| `hermes-gateway` | 127.0.0.1:8642 | Hermes Agent API server |
| `jarvis-voice` | 0.0.0.0:**8765** | voice WS `/ws` + browser HUD |

Secrets live in `~/.hermes/.env` on the LXC (the committed `server.yaml` only
contains env-var **names** and paths). Deployment, restart order and latency
tuning are in [`BRIDGE_NOTES.md`](BRIDGE_NOTES.md).

The panel connects to `ws://192.0.2.10:8765/ws`.

---

## Firmware

Written in C against **ESP-IDF v5.5.x**, using these ESP component-registry
drivers: `esp_lcd_jd9365`, `es8311` + `esp_codec_dev`, `esp_websocket_client`.

| Module | Responsibility |
|---|---|
| `main.c` | boot orchestration (NVS → netif → BSP → tasks → WS → triggers) |
| `jarvis_ws.c` | bridge client: fragmented-text reassembly, event dispatch, TTS PCM |
| `jarvis_audio.c` | capture (RMS silence gate) + playback (stream-buffer ring) |
| `jarvis_turn.c` | listening / thinking / speaking transitions, barge-in, approvals |
| `jarvis_ui.c` | arc-reactor + transcript UI, direct RGB565 framebuffer |
| `jarvis_state.c` | the shared snapshot every task reads |
| `jarvis_wifi.c` | idempotent netif/event-loop init + ESP-Hosted station |
| `jarvis_wake.c` | triggers: touch tap + serial console |
| `jarvis_ota.c` | over-the-air updates: version check + `esp_https_ota` + rollback |
| `bsp_guition/` | board support: display, touch, audio, LED, battery |

**Design notes**

* The UI renders **directly into the MIPI-DSI framebuffer** — one RGB565 buffer
  the panel scans continuously. A flush is a cache write-back, not a copy. No
  LVGL in v1 (the `ui_*` API keeps that swappable).
* The **GSL3680 touch controller needs a firmware blob** loaded at boot; it is
  embedded in `components/bsp_guition/gsl3680_fw.c`.
* Wi-Fi is **off by default**; the satellite runs fully offline until the C6
  slave firmware is set up (see [`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md) §5).
* The **WS2812** reactor LED is gated off by default (data GPIO unverified).
* **OTA** uses `esp_https_ota` against a release URL, with bootloader rollback.
  It needs the network, so it stays dormant until ESP-Hosted (Wi-Fi) is enabled;
  building with the C6 off is unaffected.

---

## Building

### Prerequisites

* **Python 3.8+** and **PlatformIO Core** (`pio`):
  ```bash
  pip install -U platformio
  # or: curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o get-platformio.py && python3 get-platformio.py
  ```
* **~2 GB free disk** — the first build downloads the pioarduino platform and
  the ESP-IDF v5.5.x RISC-V toolchain.
* A **USB-C data cable** (not charge-only) for flashing.

> ESP32-P4 support requires the **pioarduino** platform fork
> (`pioarduino/platform-espressif32`). Upstream `platformio/espressif32` has no
> P4 board/toolchain. `platformio.ini` already points at the right one, so `pio`
> fetches it automatically.

### Compile

```bash
cd firmware
pio run -e jarvis-panel
```

Output:

```
.pio/build/jarvis-panel/firmware.bin           # application image
.pio/build/jarvis-panel/firmware.factory.bin   # combined image (see below)
```

A successful build looks like:

```
RAM:   [          ]   4.8% (used 36716 bytes from 768000 bytes)
Flash: [==        ]  24.4% (used 974874 bytes from 3997696 bytes)
========================= [SUCCESS] =========================
```

---

## Flashing the panel

The P4 exposes a built-in **USB-Serial-JTAG** interface, so no external
programmer is needed — just the USB-C cable.

Three ways to flash, pick any:

### Option A — with a browser (no toolchain install)

A ready-made [ESP Web Tools](https://esphome.github.io/esp-web-tools/) page
(the same installer tech ESPHome uses) lives in [`flash/`](flash/). It detects
the panel, erases it, and writes the merged image — entirely from the browser.

```bash
git clone https://github.com/ALeXXBody/jarvis-assistant.git
cd jarvis-assistant/flash
./get-latest.sh              # fetch firmware.factory.bin from the latest release
python3 -m http.server 8000  # serve the folder
# open http://localhost:8000 in Chrome / Edge / Opera → Connect → Install
```

* Needs a **Chromium-based desktop browser** (Web Serial). Firefox/Safari don't
  work; `http://localhost` is fine.
* Once this repository is **public**, `.github/workflows/flasher.yml` also
  publishes this page to GitHub Pages on every release — then it's a plain
  one-click URL, no clone needed. (`manifest.json` and the `.bin` must be
  served from the same origin: GitHub release assets are not CORS-readable,
  which is why the page carries the image rather than linking it.)
* A **manual fallback** is on the same page: Espressif's official
  [ESP Tool (esptool-js)](https://espressif.github.io/esptool-js/) or
  [esp.huhn.me](https://esp.huhn.me) — add `firmware.factory.bin`
  (from the release) at **address 0x0**, leave the three flash settings on
  **keep** (the merged image already carries qio / 80 MHz / 16 MB in its
  header), Program.

Details and troubleshooting: [`flash/README.md`](flash/README.md).

### Option B — with PlatformIO (preferred for development)

### 1. Connect

1. Plug the panel into your computer with a **data-capable** USB-C cable.
2. Power the panel from a **≥ 2 A 5 V** supply (a weak supply can brown out
   during erase/flash).
3. Find the serial port:

   | OS | Port |
   |---|---|
   | Linux | `/dev/ttyACM0` (or `/dev/ttyUSB0`) |
   | macOS | `/dev/cu.usbmodem*` |
   | Windows | `COMx` (install the USB-Serial-JTAG driver if needed) |

   ```bash
   # Linux
   ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
   # or ask PlatformIO to list candidate ports
   pio device list
   ```

> If the port never appears, or a flash fails with `Failed to connect`, the
> board may be in a locked state: **hold `BOOT`, tap `RESET`/`PWR`, release
> `BOOT`** to force ROM download mode, then retry.

### 2. Flash

The simplest path — PlatformIO flashes the bootloader, the partition table and
the app to their correct offsets automatically:

```bash
cd firmware
pio run -e jarvis-panel -t upload
```

Expected addresses:

```
bootloader.bin   -> 0x2000
partitions.bin   -> 0x9000
firmware.bin     -> 0x20000   (ota_0)
```

Initial flash of the combined image is slower; subsequent uploads only rewrite
the app.

### 3. Clean / first-time flash (recommended on a used board)

Wipe whatever is on the board first (old vendor demo, stale NVS, etc.), then
flash:

```bash
pio run -e jarvis-panel -t erase      # full chip erase
pio run -e jarvis-panel -t upload     # bootloader + table + app
```

### 4. Alternative: one-shot flash with `esptool`

A single combined image is emitted for convenience — useful for a factory-fresh
board, or when you only have `esptool`:

```bash
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 1500000 \
    write_flash 0x0 .pio/build/jarvis-panel/firmware.factory.bin
```

(`firmware.factory.bin` bundles bootloader + partition table + ota_data + app,
so it is written whole at `0x0`. Use `esptool.py` from the PlatformIO packages,
or `pip install esptool`.)

### 5. Monitor the serial console

```bash
pio device monitor -e jarvis-panel        # 115200 baud
# or: pio device monitor -b 115200 -p /dev/ttyACM0
```

---

## Updating over the air (OTA)

Once a panel has been flashed once, it never needs a cable again. Each GitHub
release publishes two images plus a version string:

| Asset | Used for |
|---|---|
| `firmware.factory.bin` | full image for a **first-time** USB flash (`esptool`, offset `0x0`) |
| `firmware.bin` | the **app image** the panel pulls for OTA |
| `version.txt` | bare version string used for the "is an update available?" check |

### How the panel updates

* **On boot** it fetches `version.txt` from its OTA source (default: this repo's
  `releases/latest/download/`), compares it with the version compiled into the
  running image (`JARVIS_FW_VERSION`), and updates only if the release is newer.
* **On demand** — press `u` on the serial console to force a check.
* The new image is written to the **inactive** OTA slot and the panel reboots
  into it.
* **Rollback safety** — the fresh image boots in *pending verify*; if it does not
  confirm itself healthy, the bootloader automatically reverts to the previous
  slot. A bad update cannot brick the panel.

```
release ──► version.txt  ──► compare with running fw
            firmware.bin ──► write inactive slot ──► reboot ──► mark valid
```

### Publishing a release

```bash
cd firmware
tools/release.sh v0.2.0 "Display timing fix; touch axis correction"
```

This stamps the version into the build, writes the assets to `firmware/dist/`,
and creates the GitHub release with `gh`. To point a panel at a different source
(a mirror, or your own server), override `JARVIS_DEFAULT_OTA_URL` — see
[Configuration](#configuration).

> **Private repos.** GitHub serves release assets to anonymous requests only
> once the repository is public. OTA works out of the box after this repo is
> made public; until then, self-host the three assets and build the panel with
> `JARVIS_DEFAULT_OTA_URL` pointing at them.

### First-time (cable) flash from a release

Download `firmware.factory.bin` from the release and write it in one shot — no
PlatformIO needed. Either with `esptool`:

```bash
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 1500000 \
    write_flash 0x0 firmware.factory.bin
```

or entirely in the browser with the [`flash/`](flash/) page above (Option A),
which needs no command-line tool at all.

---

## First boot & verification

On a healthy boot the serial log shows (abridged):

```
 I (xxx) jarvis: ==================================================
 I (xxx) jarvis:  J.A.R.V.I.S. satellite  fw 0.1.0
 I (xxx) jarvis:  chip=... cores=2 ... flash=16MB psram=32768KB
 I (xxx) jarvis: ==================================================
 I (xxx) bsp:    I2C0 up (sda=7 scl=8)
 I (xxx) bsp_disp: MIPI-DSI PHY powered (LDO3 @ 2500mV)
 I (xxx) bsp_disp: JD9365 800x1280 RGB565 up, fb=...
 I (xxx) bsp_touch: loading GSL3680 firmware (132 blocks)
 I (xxx) bsp_touch: GSL3680 ready (addr 0x40)
 I (xxx) bsp_audio: ES8311 up @ 16000 Hz mono, vol 70
 I (xxx) ws:     connected
 I (xxx) jarvis: boot complete: state=IDLE server=ws://192.0.2.10:8765/ws
```

Verify:

1. **Backlight + UI** — the arc reactor and a status line appear on the panel.
2. **Touch** — tap the screen; the state should switch to `LISTENING`.
3. **Console trigger** — press `l` in the monitor to start listening.
4. **Full turn** — speak; the reply should play through the speaker and the
   transcript/reply should render on screen.

Serial console keys: `l` listen · `s` stop · `x` abort · `a` allow ·
`d` deny · `r` reconnect WS · `u` check for update · `+`/`-` volume.
Full bring-up checklist + fault table: [`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md).

---

## Configuration

> **This repo ships no private data.** Addresses, host names and paths in the
> docs are placeholders (the `192.0.2.x` block is IANA's documentation range).
> Point the firmware and bridge at *your* LAN.

* **Brain URL** — `JARVIS_DEFAULT_SERVER` in `firmware/src/jarvis_config.h`
  ships as a placeholder (`ws://192.0.2.10:8765/ws`). Point it at *your* bridge
  **without committing your IP** — either override at build time via the
  `PLATFORMIO_BUILD_FLAGS` environment variable:

  ```sh
  PLATFORMIO_BUILD_FLAGS='-D JARVIS_DEFAULT_SERVER=\"ws://192.0.2.10:8765/ws\"' \
      pio run -e jarvis-panel -t upload
  ```

  or edit `firmware/src/jarvis_config.h` locally and simply don't commit that
  line. (The setter `jarvis_settings_set_server()` exists for a future runtime
  console command, but v1 has no way to change the URL without a rebuild.)

* **OTA source** — `JARVIS_DEFAULT_OTA_URL` in `firmware/src/jarvis_config.h`
  defaults to this repo's GitHub `releases/latest/download/`. Override it for a
  mirror or self-hosted server, or set it to `""` to disable updating:

  ```sh
  PLATFORMIO_BUILD_FLAGS='-D JARVIS_DEFAULT_OTA_URL=\"http://192.0.2.10:8000/fw/\"' \
      pio run -e jarvis-panel
  ```

  The URL must point at a **directory**; the updater appends `version.txt` and
  `firmware.bin`. The (optional) runtime value lives in NVS and can be changed
  with `jarvis_settings_set_ota_url()`.

  > Keep the `\"…\"` escaping in these examples: PlatformIO parses
  > `PLATFORMIO_BUILD_FLAGS` with `shlex`, so plain `"…"` quotes are stripped and
  > the string macro ends up malformed.

* **Wi-Fi** — off by default. Enable ESP-Hosted and set SSID/password per
  [`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md) §5.
* **Bridge** — `bridge/server.yaml` (ports, models, personas, voices). Secrets
  stay in `~/.hermes/.env` on the bridge host (the committed `server.yaml`
  contains env-var **names** and paths only).

---

## Documentation

| Document | Contents |
|---|---|
| [`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md) | bring-up order, per-subsystem notes, pin map, partition rationale, fault table |
| [`firmware/docs/PROTOCOL.md`](firmware/docs/PROTOCOL.md) | the panel ⇄ bridge WebSocket contract |
| [`firmware/README.md`](firmware/README.md) | firmware-focused build/flash notes |
| [`PANEL_RESEARCH.md`](PANEL_RESEARCH.md) | hardware research, spec sheet, architecture options |
| [`BRIDGE_NOTES.md`](BRIDGE_NOTES.md) | brain deployment, services, restart order, latency tuning |
