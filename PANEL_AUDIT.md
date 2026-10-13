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
  ESP-Hosted's default `CONFIG_ESP_HOSTED_HOST_RESET_GPIO=54` toggles) is
  physically NC. The toggle is therefore a no-op — harmless, and the C6
  slave firmware handshake has been field-confirmed working
  (`host=3.x ↔ coprocessor=2.3.2`, the vendor's own slave image).
- W25Q128 flash drawn in the schematic; the fitted part is BOYA (boot log
  warning) — benign, generic SPI driver handles it.

## Display — JD9365 (2_LCD.png + vendor v9 driver)

Vendored driver `components/vendor_lcd_jd9365` matches the vendor v9
NewPanel driver: init table, 80 MHz DPI (hs 20/20/40, vs 4/10/30),
1500 Mbps lanes, both-axes mirror after init, 800×1280 @ 60 Hz.
Field-verified: full image, margins gone (v0.1.4+).

## Touch — GS L36xx (7_CODEC + vendor touch driver)

Vendor config for this exact board:
`esp_lcd_touch_config_t{ .swap_xy=0, .mirror_x=1, .mirror_y=1 }` with the
demo app doing one extra `x = W-1-x` ⇒ **effective: x = raw, y = V-1-raw,
no swap, no scale** (the GSL reports pixel coordinates natively on this
panel).

Firmware v0.1.12 brought the load path to vendor parity (their verbatim
4587-record firmware array + their register sequence + 0xB0 verify);
v0.1.13 adopts the vendor-effective coordinate mapping (the previous
ESPHome-style swap+rescale produced the garbage coordinates in the
v0.1.3-era tap log).

## Opening items after this audit

1. Touch on-device: confirm taps + `touch` probe → `0xb0` OK + taps
   counter rising (v0.1.13).
2. Audio end-to-end: needs the brain link (Wi-Fi provisioning first:
   console `w / srv`), then a real turn through the bridge.
3. WS2812 status LED: pin now schematic-correct (26); the driver stays
   compile-gated until first light is wanted (enable `-D JARVIS_ENABLE_WS2812`
   + `espressif/led_strip`).
