/*
 * 10.1" JD9365 MIPI-DSI panel bring-up (800x1280, RGB565) for the
 * GUITION JC8012P4A1C. Mirrors the esp_lcd_jd9365 reference bring-up:
 *
 *   LDO ch3 (2.5V) -> DSI PHY power
 *   esp_lcd_new_dsi_bus -> esp_lcd_new_panel_io_dbi -> esp_lcd_new_panel_jd9365
 *   reset -> init -> disp_on_off(true)
 *
 * num_fbs = 1 (see JD9365_800_1280_PANEL_60HZ_DPI_CONFIG): the panel scans one
 * PSRAM framebuffer continuously and the app draws straight into it. Flushing
 * a dirty rectangle means calling esp_lcd_panel_draw_bitmap() with the panel's
 * own framebuffer pointer, which the DPI driver recognises and turns into a
 * pure CPU-cache write-back (no copy).
 */
#include "bsp_guition.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_jd9365.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

#define BSP_LCD_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BSP_LCD_LEDC_TIMER   LEDC_TIMER_0
#define BSP_LCD_LEDC_CH      LEDC_CHANNEL_0
#define BSP_LCD_LEDC_RES     LEDC_TIMER_10_BIT
#define BSP_LCD_LEDC_FREQ_HZ 5000
#define BSP_LCD_LEDC_MAX     ((1u << 10) - 1)

/* MIPI DSI PHY is powered by an internal LDO on the P4 (channel 3, 2.5 V). */
#define BSP_MIPI_PHY_LDO_CHAN       3
#define BSP_MIPI_PHY_LDO_VOLTAGE_MV 2500

static esp_ldo_channel_handle_t  s_ldo;
static esp_lcd_dsi_bus_handle_t  s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t    s_panel;
static uint16_t                 *s_fbs[2];   /* two panel framebuffers (as the vendor demo) */
static int                       s_shown;    /* index of the buffer the panel scans */
static bool                      s_ready;

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = BSP_LCD_LEDC_MODE,
        .duty_resolution = BSP_LCD_LEDC_RES,
        .timer_num = BSP_LCD_LEDC_TIMER,
        .freq_hz = BSP_LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer_config");

    ledc_channel_config_t ccfg = {
        .gpio_num = BSP_LCD_BACKLIGHT_GPIO,
        .speed_mode = BSP_LCD_LEDC_MODE,
        .channel = BSP_LCD_LEDC_CH,
        .timer_sel = BSP_LCD_LEDC_TIMER,
        .duty = 0,          /* off until the panel is up */
        .hpoint = 0,
    };
    return ledc_channel_config(&ccfg);
}

void bsp_display_backlight(bool on)
{
    ledc_set_duty(BSP_LCD_LEDC_MODE, BSP_LCD_LEDC_CH, on ? BSP_LCD_LEDC_MAX : 0);
    ledc_update_duty(BSP_LCD_LEDC_MODE, BSP_LCD_LEDC_CH);
}

esp_err_t bsp_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    esp_ldo_channel_config_t ldo = {
        .chan_id = BSP_MIPI_PHY_LDO_CHAN,
        .voltage_mv = BSP_MIPI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &s_ldo), TAG, "mipi phy ldo");
    ESP_LOGI(TAG, "MIPI-DSI PHY powered (LDO%d @ %dmV)",
             BSP_MIPI_PHY_LDO_CHAN, BSP_MIPI_PHY_LDO_VOLTAGE_MV);

    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus), TAG, "new_dsi_bus");

    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io),
                        TAG, "new_panel_io_dbi");

    esp_lcd_dpi_panel_config_t dpi_config =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_config.num_fbs = 2;   /* draw one while the panel scans the other */
    jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = 2,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_BITS,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(s_dbi_io, &panel_config, &s_panel),
                        TAG, "new_panel_jd9365");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");
    /* The vendor's NewPanel bring-up mirrors both axes after init. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1),
                        TAG, "get_fb");
    s_fbs[0] = (uint16_t *)fb0;
    s_fbs[1] = (uint16_t *)fb1;
    s_shown = 0;   /* the driver starts scanning buffer 0 */

    bsp_display_backlight(true);

    s_ready = true;
    ESP_LOGI(TAG, "JD9365 %dx%d RGB565 up, fb=%p/%p", BSP_LCD_H_RES, BSP_LCD_V_RES,
             (void *)s_fbs[0], (void *)s_fbs[1]);
    return ESP_OK;
}

/* The buffer NOT on screen: draw into this one. */
uint16_t *bsp_display_framebuffer(void)
{
    return s_fbs[s_shown ^ 1];
}

/* Present the back buffer. The driver switches the scan to it at the next
 * frame boundary (its DMA restarts from cur_fb_index), so the panel never
 * shows a half-drawn frame. */
esp_err_t bsp_display_flush(int x0, int y0, int x1, int y1)
{
    if (!s_panel || !s_fbs[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    int next = s_shown ^ 1;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_fbs[next]);
    if (err == ESP_OK) {
        s_shown = next;
    }
    return err;
}

void bsp_display_selftest(void)
{
    /* Diagnostic (see main.c: runs when JARVIS_DISPLAY_SELFTEST is defined):
     * cycles solid colours on the raw framebuffer for a few seconds. Seeing
     * them => panel, DSI link, DPI timings and the flush path are all fine.
     * Staying black => panel init / power / LDO / reset / DSI signal. */
    if (!s_panel || !s_fbs[0]) {
        ESP_LOGW(TAG, "selftest skipped: no framebuffer");
        return;
    }
    ESP_LOGW(TAG, "DISPLAY SELFTEST: colour cycle");
    static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    static const char *const names[] = { "red", "green", "blue", "white" };
    for (int round = 0; round < 2; round++) {
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
            ESP_LOGI(TAG, "selftest: %s", names[i]);
            uint16_t *fb = bsp_display_framebuffer();
            for (size_t p = 0; p < (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES; p++) {
                fb[p] = colors[i];
            }
            bsp_display_flush(0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES);
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }
}
