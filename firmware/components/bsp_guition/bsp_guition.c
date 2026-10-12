/*
 * Board bring-up: the shared I2C master bus used by the touch controller and
 * the ES8311 audio codec. Everything else lives in bsp_display.c / bsp_touch.c
 * / bsp_audio.c / bsp_led.c / bsp_sys.c.
 */
#include "bsp_guition.h"

#include "esp_log.h"

static const char *TAG = "bsp";

static i2c_master_bus_handle_t s_i2c;

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;   /* idempotent */
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_MASTER_NUM,
        .sda_io_num = BSP_TOUCH_I2C_SDA,
        .scl_io_num = BSP_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C%d up (sda=%d scl=%d)", BSP_I2C_MASTER_NUM,
             BSP_TOUCH_I2C_SDA, BSP_TOUCH_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return s_i2c;
}
