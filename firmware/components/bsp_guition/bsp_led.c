/*
 * Reactor status LED (WS2812 / addressable single pixel).
 *
 * DISABLED BY DEFAULT. The WS2812 data GPIO for this board is not yet verified
 * against the vendor schematic (PANEL_RESEARCH.md lists GPIO0 as a guess), and
 * driving an unknown GPIO can disturb other on-board functions. Enable by
 * adding -D JARVIS_ENABLE_WS2812 to build_flags in platformio.ini and adding
 * `espressif/led_strip` to src/idf_component.yml once the pin is confirmed.
 *
 * With the feature off the app still gets a no-op bsp_led API, and the arc
 * reactor on the LCD carries all the status feedback.
 */
#include "bsp_guition.h"

#include "esp_log.h"

static const char *TAG = "bsp_led";

#ifdef JARVIS_ENABLE_WS2812

#include "led_strip.h"
#include "esp_check.h"

static led_strip_handle_t s_strip;

esp_err_t bsp_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = false },
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip),
                        TAG, "led_strip_new_rmt_device");
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "WS2812 on GPIO%d", BSP_WS2812_GPIO);
    return ESP_OK;
}

void bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

#else  /* JARVIS_ENABLE_WS2812 */

esp_err_t bsp_led_init(void)
{
    ESP_LOGI(TAG, "WS2812 disabled (pin unverified; define JARVIS_ENABLE_WS2812 to enable)");
    return ESP_OK;
}

void bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r; (void)g; (void)b;
}

#endif /* JARVIS_ENABLE_WS2812 */
