# JARVIS satellite — hardware bring-up

Target board: **GUITION JC8012P4A1C_I_W_Y** — ESP32-P4 + on-board ESP32-C6,
10.1" JD9365 MIPI-DSI panel (800×1280), GSL3680 touch, ES8311 codec.

This document is the checklist for first power-on and for debugging each
subsystem. The pin map and controller details come from `PANEL_RESEARCH.md`
(community/vendor-BSP verified) and were re-checked against the ESP component
registry driver headers during the build.

## 0. Build & flash

```bash
cd firmware
pio run -e jarvis-panel                 # build
pio run -e jarvis-panel -t upload       # flash (USB-C)
pio device monitor -e jarvis-panel      # serial console @115200
```

The build emits a combined image `firmware.factory.bin` (bootloader + table +
ota_data + app) for one-shot flashing at `0x0`:

```bash
esptool.py --chip esp32p4 write_flash 0x0 .pio/build/jarvis-panel/firmware.factory.bin
```

Verified build footprint: Flash ≈ 24 %, static RAM ≈ 4.8 %.

Air updates are enabled (see §4a); after the cable flash above you can push new
builds as GitHub releases instead of reflashing.

## 1. Pin map

| Function | Signal | GPIO |
|---|---|---|
| Display (JD9365, 2-lane MIPI-DSI) | reset | 27 |
| | backlight (LEDC, active-high) | 23 |
| Touch (GSL3680, I²C 0x40) | SDA | 7 |
| | SCL | 8 |
| | reset | 22 |
| | interrupt | 21 |
| Audio (ES8311) | MCLK | 13 |
| | BCLK | 12 |
| | LRCLK/WS | 10 |
| | DOUT (P4→codec) | 9 |
| | DIN (codec→P4) | 11 |
| | PA enable | 20 |
| microSD | CLK/CMD/D0–D3 | 43/44/39/40/41/42 |
| ESP32-C6 (ESP-Hosted SDIO) | CLK/CMD/D0–D3/reset | 18/19/14/15/16/17/54 |
| Battery | divider sense (÷1.68) | 52 |
| Reactor LED (WS2812) | data | 0 *(unverified)* |

Shared I²C bus: **port 0** (touch + codec).

## 2. Bring-up order

`main.c` initialises in this order; each peripheral failure is logged and
**non-fatal** so the serial console and WS link still come up on a bench board:

1. NVS (`nvs_flash_init`, erase-and-retry on version mismatch) — fatal if it fails.
2. Settings (NVS-backed) + state store.
3. `jarvis_net_init()` → `esp_netif_init` + default event loop (idempotent).
   Required by the WS client even when Wi-Fi is off.
4. BSP: display → touch → LED → battery.
5. UI task, audio tasks, turn engine.
6. WS client to `cfg->server_url`.
7. Wi-Fi (ESP-Hosted) — expected to fail until the C6 is set up (see §5).
8. Trigger tasks (touch tap + serial console).

## 3. Per-subsystem notes

### Display (JD9365)
* **The MIPI D-PHY must be powered before DSI init**: the driver acquires LDO
  channel 3 at 2500 mV (`esp_ldo_acquire_channel`).
* Single framebuffer, direct-render. `bsp_display_framebuffer()` returns a
  writable 800×1280 RGB565 buffer that the panel scans continuously.
* After drawing, call `bsp_display_flush(0,0,W,H)`. Because the flush buffer is
  the panel's own framebuffer, the driver only performs a **cache write-back**
  (`esp_cache_msync … C2M`), no copy.
* If the picture is inverted or shifted, adjust
  `JD9365_800_1280_PANEL_60HZ_DPI_CONFIG` / vendor init in `bsp_display.c`.
* Backlight is LEDC on GPIO23; `bsp_display_backlight(false)` blanks it.

### Touch (GSL3680)
* The controller needs a **firmware blob** loaded at boot (132 × 132-byte
  blocks). It is embedded in `components/bsp_guition/gsl3680_fw.c` from
  `esphome-libs/gsl3670-firmware` v1.0.0 `seeed-d1001-fw.bin`
  (sha256 `2e50501a…127a4`, 17424 bytes).
* Init sequence: clear_reg → reset → load_fw → startup → reset → startup.
* Raw coordinate window is x∈[20,880], y∈[20,1648]; the driver applies
  `swap_xy=true, mirror_x=true, mirror_y=false` then scales to 800×1280.
* If axes are wrong, flip the transform flags in `bsp_touch.c`.

### Audio (ES8311)
* Data path goes through **esp_codec_dev** (not the standalone `es8311`
  legacy-I2C driver, which would fight the new `i2c_master` bus). The component
  owns the std I2S driver: we create both I2S0 channel handles, hand them to
  `audio_codec_new_i2s_data()`, then `esp_codec_dev_open()` performs
  `set_fmt` + `enable`.
* Format: 16-bit mono @ 16 kHz, MCLK ×256. The bridge expects the same.
* `esp_codec_dev_open` needs `CONFIG_CODEC_ES8311_SUPPORT=y` (set by the
  registry component; already enabled).
* Codec I²C address is `ES8311_CODEC_DEFAULT_ADDR` (0x30); the ctrl layer
  shifts `>>1` → 7-bit 0x18 on the bus.

### Battery
* ADC2 channel 3 = GPIO52, atten 12 dB. Voltage is reported at the battery
  terminal (`raw_mV × 1.68`). Falls back to raw scaling if no calibration
  scheme is available.

### Reactor LED (WS2812) — disabled by default
* The data GPIO (0) is **unverified**. Driving an unknown pin can disturb other
  on-board functions, so the driver is compiled out unless you add
  `-D JARVIS_ENABLE_WS2812` to `build_flags` and add `espressif/led_strip` to
  `src/idf_component.yml`. The on-screen arc reactor carries all status
  feedback regardless.

## 4. Partition table

16 MB layout (`partitions.csv`):

| Name | Type/Sub | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0xa000 | 0x6000 |
| otadata | data/ota | 0x10000 | 0x2000 |
| phy_init | data/phy | 0x12000 | 0x1000 |
| ota_0 | app/ota_0 | 0x20000 | 0x3D0000 |
| ota_1 | app/ota_1 | 0x3F0000 | 0x3D0000 |
| assets | data/spiffs | 0x7C0000 | 0x840000 |

**Why the odd offsets:** the ESP32-**P4** bootloader (~24 KB @ 0x2000) runs past
0x8000, so `CONFIG_PARTITION_TABLE_OFFSET` is 0x9000 and nvs starts at 0xa000.
PlatformIO picks the app load address from the `ota_0` subtype first, so the
primary slot is named `ota_0` (at 0x20000) — not `factory`.

## 4a. OTA updates

The two `ota_*` slots are used for over-the-air updates (`src/jarvis_ota.c`):

* The panel fetches `version.txt` + `firmware.bin` from a release URL
  (`JARVIS_DEFAULT_OTA_URL`), compares versions, and writes the new image to the
  **inactive** slot, then reboots into it.
* `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (plus the full mbedTLS CA bundle for
  GitHub HTTPS and `CONFIG_OTA_ALLOW_HTTP` for LAN mirrors) is set in
  `sdkconfig.defaults`.
* A fresh OTA image boots in **pending verify**. `main.c` calls
  `jarvis_ota_mark_valid()` once bring-up completes, which cancels the rollback;
  if the image crashes first, the bootloader reverts to the previous slot. The
  serial log prints `(PENDING VERIFY)` while an image is unconfirmed.
* Trigger a check manually with `u` on the console; `tools/release.sh` publishes
  the assets. Details: [root README](../../README.md#updating-over-the-air-ota).

## 5. Wi-Fi (ESP32-P4 has no radio)

Wi-Fi is provided by the on-board **ESP32-C6** via ESP-Hosted over SDIO.
Until the C6 slave firmware is flashed, the satellite runs **offline**:
the UI, touch and audio all work; only the WS link to the brain is missing.

To enable:

1. Flash the matching ESP-Hosted **slave** firmware to the C6 (see the Espressif
   ESP-Hosted docs for P4 + C6 SDIO).
2. Uncomment `espressif/esp_hosted` in `src/idf_component.yml`.
3. Enable the `CONFIG_ESP_HOSTED_*` / `CONFIG_ESP_WIFI_REMOTE_ENABLED` block in
   `sdkconfig.defaults` and set `-D JARVIS_HAVE_WIFI=1`.
4. Set SSID/password in NVS (console command, see below) or `jarvis_config.h`.

## 6. Serial console commands

Single keypresses on the monitor (`jarvis_wake.c`):

| Key | Action |
|---|---|
| `l` | start listening (begin an utterance) |
| `s` | stop listening → send `stop` (run the turn) |
| `x` | abort the current turn (`stop_run` + flush audio) |
| `a` | approve a pending `approval_request` |
| `d` | deny a pending `approval_request` |
| `r` | (re)start the WebSocket client |
| `u` | check for a firmware update (OTA) |
| `+` / `-` | volume up / down (persisted to NVS) |

A **touch tap** anywhere on the panel does the same as `l` (toggle: starts
listening, or stops/aborts depending on the current state) — see
`jarvis_turn_toggle()`.

## 7. Quick fault table

| Symptom | Likely cause / check |
|---|---|
| Blank screen, backlight on | MIPI LDO not acquired; check init log + JD9365 config |
| Blank screen, backlight off | `bsp_display_init` failed early — read the warning line |
| Touch dead | firmware blob not loaded, or I²C probe fails (check addr 0x40) |
| Touch mirrored/rotated | transform flags in `bsp_touch.c` |
| No mic / no audio | ES8311 not opening: check I²C addr + `esp_codec_dev_open` log |
| WS keeps dropping | server reachable? ping timeout tuning in `jarvis_ws.c` |
| WS connects, no reply | bridge services up on the LXC? see `BRIDGE_NOTES.md` |
| `u` says "could not read version.txt" | repo still private, no release asset, or no network |
| Panel reboots into old firmware | new image never confirmed — it crashed before `jarvis_ota_mark_valid()`; rollback worked |
