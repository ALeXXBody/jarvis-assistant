# Board audit — vendor-package analysis (JC8012P4A1C_I_W_Y.zip)

Systematic pass over the vendor archive (`vendor/JC8012P4A1C_I_W_Y.zip`),
comparing its demo source (`1-Demo/arduino-examples/..._NewPanel/esp32p4_lvgl_v9`)
and schematics (`5-Schematic/*.png`) against our firmware, subsystem by
subsystem. Date: 2026-10-13.

## Pin map — schematic-verified (`5-Schematic/3_ESP32-P4.png`)

| Function | Net | P4 GPIO | Our firmware |
|---|---|---|---|
| LCD backlight (LED_PWM) | LED PWM | **23** | 23 ✓ |
| LCD reset | | **27** | 27 ✓ |
| Touch I2C SDA / SCL | TP_SDA/TP_SCL | **7 / 8** | 7 / 8 ✓ |
| Touch INT / RST | TP_INT / TP_RST | **21 / 22** | 21 / 22 ✓ |
| Touch chip addr | | | 0x40 ✓ |
| PA (NS4150 speaker amp) control | PA_CTRL | **20** | 20 ✓ |
| Codec I2S: DSDIN (P4→DAC) | CODEC I2SC DSDIN | **9** | 9 ✓ |
| Codec I2S SCLK | BCLK | **12** | 12 ✓ |
| Codec / ES7210 LRCK | LRCK | **10** | 10 ✓ |
| Mic data in | ES7210 SDOUT (net also on ES8311 ASDOUT) | **11** | 11 ✓ |
| MCLK | MCLK | **13** | 13 ✓ |
| WS2812 status LED | WS2812_DAT | **26** | was 0 ✗ → fixed to 26 |
| UART0 (serial console) | UART0_TXD/RXD | **69/70** | UART0 ✓ |

Schematic notes:
- The analog MEMS mic (MSM381) feeds the **ES8311 differential input**
  (MIC1P/N) with MICBIAS from ES8311 ADCVREF — our capture path is the
  right one; no ES7210 in the capture chain.
- **C6_CHIP_PU is NOT driven by the P4.** It exists only on debug
  connector CN5 (pin 6). The C6 boots by itself; package pin 54 (the pin
  ESP-Hosted's `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54` toggles) is
  physically NC. The toggle is therefore a no-op — harmless, and the C6
  slave firmware handshake is field-confirmed working (the vendor's own
  `JC-C6-slave` image, which reports coprocessor fw 2.3.2).
- W25Q128 flash drawn in the schematic; the fitted part is BOYA (boot log
  warning) — benign, generic SPI driver handles it.

## Display — JD9365 (2_LCD.png + vendor v9 driver)

Vendored driver `firmware/components/vendor_lcd_jd9365` matches the vendor v9
NewPanel driver: init table, 80 MHz DPI (hs 20/20/40, vs 4/10/30), 1500 Mbps
lanes, both-axis mirror after init, 800x1280 at 60 Hz. Verified on the panel:
full image, no dark margins.

Open: vertical white lines were reported on an early build and not re-checked.

## Touch — GSL3680 (vendor touch driver + field logs)

- Bring-up: vendor sequence (clear, reset, load 4587 firmware records, startup)
  with the 0xB0 == 5A5A5A5A verify. Verified on the panel.
- Coordinates: X follows the chip's raw axis with no flip; Y is flipped
  (`sy = V-1-dy`). **Field-confirmed on the panel** — corner and edge taps land
  directly under the finger across the full 800x1280 area.
- The tap log prints mapped and raw values: `tap at X,Y (raw RX,RY)`.

## Audio — ES8311 (7_CODEC.png)

- Analog mic on MIC1P/MIC1N (differential), I2S on GPIO 9/10/11/12/13,
  PA enable on GPIO 20, shared I2C on GPIO 7/8. Matches the firmware.
- Microphone input gain was never set. It is now 24 dB (`BSP_AUDIO_MIC_GAIN_DB`
  in `bsp_audio.c`). **Not verified on the panel** — the `mic level` log line
  shows whether speech reaches the level threshold.

## Wi-Fi — ESP32-C6 over ESP-Hosted

- The C6 runs the vendor slave firmware, which reports coprocessor fw 2.3.2.
  ESP-Hosted logs a "major version mismatch - OTA coprocessor from host"
  advisory against it — it **only logs**; the Wi-Fi API works.
- The host is pinned to the **3.x** line (`espressif/esp_hosted` ^3.0.9),
  field-verified on the panel (station joins a 2.4 GHz WPA2 network, gets an
  IP). v0.1.9 tried to silence the advisory by pinning the host to the 2.x
  line (2.13.0) and **killed the radio**: the panel reported
  "radio unavailable" (esp_wifi_init never came up). Reverted in v0.1.10.
  Do not pin the host to 2.x again.
- SDIO wiring stays on ESP-Hosted's own ESP32-P4 defaults (bus SDIO, slot 1,
  4-bit) — the same configuration the panel was verified with from v0.1.4
  through v0.1.8.
- Join retry policy (since v0.1.11): the first 5 failed joins retry every
  3 s, then attempts slow to one every 45 s. This is what keeps the setup
  sheet's network list usable after a bad saved password: a scan against a
  radio stuck mid 4-way handshake returns 0 APs — the join loop now goes
  quiet before every scan and resumes 2 s later. The status line shows
  `wifi: password rejected` / `AP not found` so the cause stays visible.
- The `ui_scan` task stack was raised to 20 KB: the field log caught two
  stack-protection panics in exactly that task while scanning (panel
  reboots when opening the setup sheet).

## Settings persistence across updates

- Runtime settings (Wi-Fi SSID/pass, brain URL, OTA URL, volume) live in the
  `jarvis` NVS namespace inside the `nvs` partition (`0xa000`-`0x10000`).
- On-device OTA (`esp_https_ota`) writes only the inactive app slot, so NVS is
  never touched — settings already survive an over-the-air update.
- The browser installer now offers two paths:
  - **fresh install** — the merged `firmware.factory.bin` at offset 0, which
    erases the whole chip (including NVS);
  - **update** — `bootloader.bin` / `partitions.bin` / `ota_data_initial.bin` /
    `firmware.bin` written at `0x2000` / `0x9000` / `0x10000` / `0x20000`,
    which never overlaps the NVS region, so settings are preserved.

## Status of open items

1. Running firmware version — reported on the setup sheet, bottom bar and in
   the 10 s boot report. Since v0.1.10 it is read from the running image's
   app descriptor (stamped by `tools/release.sh`), so the number is the
   version of the image actually running, not a compile-time constant.
2. Microphone level — the ES8311 input gain is now 24 dB
   (`BSP_AUDIO_MIC_GAIN_DB`); awaiting a user-reported `mic level rms=...` line
   from a live turn.
3. Touch — verified on the panel (see above).
4. The brain link works (`ws://192.0.2.10:8765/ws`); the bridge answered
   `No transcript detected` for the early test turns — a bridge-side item the
   user is handling.
5. ESP-Hosted host/coprocessor version "match" — **resolved as a documented
   non-issue**. The advisory is cosmetic; matching the coprocessor by pinning
   the host to the 2.x line (v0.1.9) made the radio disappear and was
   reverted in v0.1.10. The 3.x host stays; the advisory is logged once per
   boot and Wi-Fi works.
