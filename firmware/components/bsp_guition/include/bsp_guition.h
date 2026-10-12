/*
 * bsp_guition — board support for the GUITION JC8012P4A1C_I_W_Y
 * ESP32-P4 + ESP32-C6, 10.1" JD9365 MIPI-DSI panel, GSL3680 touch,
 * ES8311 codec, WS2812 status LED.
 *
 * All board-specific pin/timing knowledge lives here so the application is
 * portable. Pin map per PANEL_RESEARCH.md §2.2 (community/BSP-verified);
 * verify against the vendor SDK once the archive is extracted.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Pin map                                                                   */
/* ------------------------------------------------------------------------- */
#define BSP_LCD_BACKLIGHT_GPIO   23
/* JD9365 panel reset is driven by the panel driver config (see bsp_display.c) */
#define BSP_LCD_RST_GPIO         27

#define BSP_TOUCH_I2C_SDA        7
#define BSP_TOUCH_I2C_SCL        8
#define BSP_TOUCH_RST_GPIO       22
#define BSP_TOUCH_INT_GPIO       21
#define BSP_TOUCH_I2C_ADDR       0x40

#define BSP_I2C_MASTER_NUM       0          /* shared I2C bus (touch + codec) */
#define JARVIS_AUDIO_I2S_PORT    0          /* I2S0, full duplex (codec) */
#define BSP_AUDIO_ES8311_ADDR    0x18

#define BSP_AUDIO_MCLK_GPIO      13
#define BSP_AUDIO_BCLK_GPIO      12
#define BSP_AUDIO_LRCLK_GPIO     10
#define BSP_AUDIO_DOUT_GPIO      9          /* P4 -> codec (playback)  */
#define BSP_AUDIO_DIN_GPIO       11         /* codec -> P4 (capture)   */
#define BSP_AUDIO_PA_GPIO        20         /* speaker amp enable      */

#define BSP_WS2812_GPIO          0          /* reactor LED (verify)    */

#define BSP_BATTERY_ADC_GPIO     52
#define BSP_BATTERY_DIVIDER      1.68f

/* ------------------------------------------------------------------------- */
/* Display                                                                   */
/* ------------------------------------------------------------------------- */
#define BSP_LCD_H_RES            800
#define BSP_LCD_V_RES            1280
#define BSP_LCD_COLOR_BITS       16         /* RGB565 */

/* Bring up MIPI-DSI panel + backlight. After a successful call,
 * bsp_display_framebuffer() returns a writable W*H RGB565 buffer the panel
 * is NOT currently scanning; bsp_display_flush() presents it (double
 * buffer). */
esp_err_t bsp_display_init(void);
uint16_t *bsp_display_framebuffer(void);
void      bsp_display_backlight(bool on);
/* Push a dirty rectangle to the panel (needed when not direct-rendering). */
esp_err_t bsp_display_flush(int x0, int y0, int x1, int y1);

/* ------------------------------------------------------------------------- */
/* Touch (GSL3680)                                                           */
/* ------------------------------------------------------------------------- */
typedef struct {
    uint16_t x;
    uint16_t y;
    bool     pressed;
} bsp_touch_point_t;

esp_err_t bsp_touch_init(void);
/* Returns true and fills |pt| when a finger is present. */
bool      bsp_touch_read(bsp_touch_point_t *pt);
/* Serial-console probe: touch counters + one raw GSL data capture. */
void      bsp_touch_diag(void);

/* ------------------------------------------------------------------------- */
/* Audio (ES8311)                                                            */
/* ------------------------------------------------------------------------- */
esp_err_t bsp_audio_init(void);
/* Capture: read up to |max| bytes of 16 kHz mono S16LE into |buf|.
 * Returns bytes read (may block briefly). */
int       bsp_audio_record(uint8_t *buf, size_t max, uint32_t timeout_ms);
/* Playback: write 16 kHz mono S16LE PCM (blocking, paced by I2S). */
esp_err_t bsp_audio_play(const uint8_t *buf, size_t len, uint32_t timeout_ms);
esp_err_t bsp_audio_set_volume(int percent);
esp_err_t bsp_audio_set_mute(bool mute);

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */
esp_err_t bsp_led_init(void);
void      bsp_led_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t bsp_battery_init(void);
/* Battery voltage in volts (0 if unavailable). */
float     bsp_battery_voltage(void);

/* Shared I2C bus (touch + codec). bsp_i2c_init() is idempotent. */
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_bus(void);

#ifdef __cplusplus
}
#endif
