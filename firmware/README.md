# JARVIS satellite firmware

Custom ESP-IDF firmware for the **GUITION JC8012P4A1C_I_W_Y** 10.1" touch panel
(ESP32-P4 + ESP32-C6), turning it into a hands-free voice terminal for a fully
local JARVIS brain.

The panel is a "hi-tech satellite": it captures mic audio, streams it to the
brain over WebSocket, plays back the spoken reply, and shows a live arc-reactor
status display with the running transcript. **No LVGL** — the UI renders
directly into the MIPI-DSI framebuffer.

## Brain (not part of this repo's build)

The intelligence lives on a separate host (`192.0.2.10`):
Hermes Agent + local Ollama + STT/TTS, exposed by the `jarvis_ai` voice bridge
at `ws://192.0.2.10:8765/ws`. See `../BRIDGE_NOTES.md` for the service
stack, and `docs/PROTOCOL.md` for the wire contract.

## Build & flash

### Prerequisites
* PlatformIO Core (`pip install -U platformio`).
* A USB-C **data** cable and a **≥ 2 A** 5 V supply.
* ~2 GB free disk for the first build (downloads the pioarduino platform +
  ESP-IDF v5.5.x RISC-V toolchain).

### Build
```bash
cd firmware
pio run -e jarvis-panel
```
Produces `firmware.bin` (app) and `firmware.factory.bin` (combined image).

### Flash
```bash
# find the port first (P4 exposes USB-Serial-JTAG)
pio device list
# first time on a used board: wipe it
pio run -e jarvis-panel -t erase
# flash bootloader + partition table + app
pio run -e jarvis-panel -t upload
# serial console @115200
pio device monitor -e jarvis-panel
```

Flash addresses: `bootloader.bin → 0x2000`, `partitions.bin → 0x9000`,
`firmware.bin → 0x20000` (`ota_0`).

One-shot alternative (whole combined image at `0x0`):
```bash
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 1500000 \
    write_flash 0x0 .pio/build/jarvis-panel/firmware.factory.bin
```

If the port never appears or flashing fails to connect: **hold `BOOT`, tap
`RESET`/`PWR`, release `BOOT`** to enter ROM download mode, then retry.

Full walkthrough, verification and fault table: [`docs/BRINGUP.md`](docs/BRINGUP.md)
and the [repo root README](../README.md#flashing-the-panel).

### Toolchain note
ESP32-P4 needs the **pioarduino** platform
(`https://github.com/pioarduino/platform-espressif32.git#develop`), which ships
ESP-IDF v5.5.x and the RISC-V toolchain. `platformio.ini` already points at it.
Board definition: `boards/gc_jc8012p4a1c.json`.

## How it works

```
        ┌────────────── esp32-p4 (this firmware) ──────────────┐
 mic ─▶ │ jarvis_audio ─ capture task (16 kHz mono S16LE)      │
        │      │                                               │
        │      ▼  binary WS frames                             │
        │ jarvis_ws ────────── ws://brain:8765/ws ─────────────▶│
        │      │  ◀───── text JSON events + binary TTS PCM ─────│
        │      ├─▶ jarvis_state (mutex-protected snapshot)      │
        │      ├─▶ jarvis_audio ─ playback task ──▶ speaker     │
        │      └─▶ jarvis_ui ─ direct RGB565 framebuffer ──▶ LCD │
        │ jarvis_turn (turn/approval state machine)            │
        │ jarvis_wake (touch tap + serial console triggers)    │
        └──────────────────────────────────────────────────────┘
```

* `jarvis_ws.c` — bridge client: reassembles fragmented text frames, dispatches
  events into state, streams TTS PCM to playback.
* `jarvis_audio.c` — capture (RMS silence gate auto-ends the utterance) and
  playback (stream-buffer ring).
* `jarvis_turn.c` — listening/thinking/speaking transitions, barge-in, approvals.
* `jarvis_ui.c` — arc reactor + transcript, 800×1280 RGB565, 8×8 ASCII font.
* `jarvis_state.c` — the shared snapshot all tasks read.
* `bsp_guition/` — board support: display, touch, audio, LED, battery
  (see `docs/BRINGUP.md`).

## Layout

```
firmware/
├── platformio.ini          build config (env: jarvis-panel)
├── sdkconfig.defaults      P4 / flash / PSRAM / partition defaults
├── partitions.csv          16 MB table (nvs/otadata/ota_0/ota_1/assets)
├── boards/                 custom pioarduino board definition
├── src/                    application (jarvis_*)
├── components/bsp_guition/ board support package + embedded touch firmware
└── docs/                   BRINGUP.md, PROTOCOL.md
```

## Status

* Builds clean (no warnings) for `esp32p4`; Flash ≈ 21 %, static RAM ≈ 4.8 %.
* Display, touch, audio, battery drivers are written against verified driver
  APIs; **not yet validated on physical hardware** — that is the next step.
* Wi-Fi (ESP-Hosted to the C6) is gated off; the satellite runs offline until
  the C6 slave firmware is set up (`docs/BRINGUP.md` §5).
* WS2812 reactor LED is gated off by default (pin unverified).
