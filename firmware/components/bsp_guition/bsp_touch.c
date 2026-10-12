/*
 * GSL3680 capacitive touch controller (I2C 0x40) on the GUITION JC8012P4A1C.
 *
 * The GSL3670/GSL3680 has no usable on-chip touch firmware: after every reset
 * the host must load a firmware blob before the chip reports touches. The
 * register protocol below mirrors the Seeed reTerminal BSP / ESPHome gsl3670
 * component, which explicitly supports this exact board model
 * ("GUITION-JC8012P4A1": reset GPIO22, int GPIO21, addr 0x40).
 *
 * Report frame (read from reg 0x80, 44 bytes):
 *   buf[0]        finger count (0..n)
 *   per finger j: base = (j+1)*4
 *     raw_y = (buf[base+1] << 8) | buf[base+0]
 *     raw_x = ((buf[base+3] & 0x0f) << 8) | buf[base+2]
 *     id    =  buf[base+3] >> 4
 *
 * Raw -> panel mapping for this model (ESPHome): swap_xy, mirror_x,
 * mirror_y=false, raw x/y spans 20..880 / 20..1648. Only "pressed" is used by
 * the v1 UI (a tap anywhere triggers a turn); the scaled coordinates are best
 * effort — verify orientation on hardware.
 */
#include "bsp_guition.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

/* Firmware blob: components/bsp_guition/gsl3680_fw.c */
extern const uint8_t gsl3680_fw[];
extern const size_t  gsl3680_fw_blocks;

#define GSL_FW_BLOCK       132           /* 4 header + 128 data bytes */
#define GSL_REG_DATA       0x80
#define GSL_DATA_LEN       44
#define GSL_MAX_FINGERS    3

/* Model-specific calibration (ESPHome GUITION-JC8012P4A1). */
#define GSL_RAW_X_MIN      20
#define GSL_RAW_X_MAX      880
#define GSL_RAW_Y_MIN      20
#define GSL_RAW_Y_MAX      1648

static i2c_master_dev_handle_t s_dev;

static esp_err_t gsl_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + GSL_FW_BLOCK];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    if (len) {
        memcpy(&buf[1], data, len);
    }
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

static esp_err_t gsl_write8(uint8_t reg, uint8_t val)
{
    return gsl_write(reg, &val, 1);
}

static void gsl_hw_reset(void)
{
    gpio_set_level(BSP_TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(BSP_TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
}

static esp_err_t gsl_clear_reg(void)
{
    gsl_hw_reset();
    ESP_RETURN_ON_ERROR(gsl_write8(0x88, 0x01), TAG, "w 0x88");
    ESP_RETURN_ON_ERROR(gsl_write8(0xe4, 0x04), TAG, "w 0xe4");
    return gsl_write8(0xe0, 0x00);
}

static esp_err_t gsl_reset(void)
{
    gsl_hw_reset();
    ESP_RETURN_ON_ERROR(gsl_write8(0xe4, 0x04), TAG, "w 0xe4");
    uint8_t zeros[4] = { 0, 0, 0, 0 };
    return gsl_write(0xbc, zeros, sizeof(zeros));
}

static esp_err_t gsl_startup(void)
{
    ESP_RETURN_ON_ERROR(gsl_write8(0xe0, 0x00), TAG, "w 0xe0");
    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}

static esp_err_t gsl_load_fw(void)
{
    ESP_LOGI(TAG, "loading GSL3680 firmware (%u blocks)", (unsigned)gsl3680_fw_blocks);
    for (size_t i = 0; i < gsl3680_fw_blocks; i++) {
        const uint8_t *blk = gsl3680_fw + i * GSL_FW_BLOCK;
        ESP_RETURN_ON_ERROR(gsl_write(0xf0, blk, 1), TAG, "fw addr");
        ESP_RETURN_ON_ERROR(gsl_write(0x00, blk + 4, 128), TAG, "fw data");
    }
    return ESP_OK;
}

esp_err_t bsp_touch_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_bus(), &dev_cfg, &s_dev),
                        TAG, "add touch device");

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << BSP_TOUCH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio");
    gpio_set_level(BSP_TOUCH_RST_GPIO, 1);

    ESP_RETURN_ON_ERROR(gsl_clear_reg(), TAG, "clear_reg");
    ESP_RETURN_ON_ERROR(gsl_reset(), TAG, "reset");
    ESP_RETURN_ON_ERROR(gsl_load_fw(), TAG, "load_fw");
    ESP_RETURN_ON_ERROR(gsl_startup(), TAG, "startup");
    ESP_RETURN_ON_ERROR(gsl_reset(), TAG, "reset2");
    ESP_RETURN_ON_ERROR(gsl_startup(), TAG, "startup2");

    ESP_LOGI(TAG, "GSL3680 ready (addr 0x%02x)", BSP_TOUCH_I2C_ADDR);
    return ESP_OK;
}

bool bsp_touch_read(bsp_touch_point_t *pt)
{
    if (!pt || !s_dev) {
        return false;
    }

    uint8_t reg = GSL_REG_DATA;
    uint8_t buf[GSL_DATA_LEN];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 100) != ESP_OK) {
        return false;
    }

    uint8_t fingers = buf[0] > GSL_MAX_FINGERS ? GSL_MAX_FINGERS : buf[0];
    if (fingers == 0) {
        pt->pressed = false;
        return false;
    }

    /* Finger 0: base = 4. */
    uint16_t raw_x = (uint16_t)(((buf[7] & 0x0f) << 8) | buf[6]);
    uint16_t raw_y = (uint16_t)((buf[5] << 8) | buf[4]);

    /* swap_xy (model config) */
    uint16_t x = raw_y;
    uint16_t y = raw_x;

    /* clamp to the model's raw window, then scale to panel pixels */
    if (x < GSL_RAW_X_MIN) x = GSL_RAW_X_MIN;
    if (x > GSL_RAW_X_MAX) x = GSL_RAW_X_MAX;
    if (y < GSL_RAW_Y_MIN) y = GSL_RAW_Y_MIN;
    if (y > GSL_RAW_Y_MAX) y = GSL_RAW_Y_MAX;

    uint32_t dx = (uint32_t)(x - GSL_RAW_X_MIN) * BSP_LCD_H_RES / (GSL_RAW_X_MAX - GSL_RAW_X_MIN);
    uint32_t dy = (uint32_t)(y - GSL_RAW_Y_MIN) * BSP_LCD_V_RES / (GSL_RAW_Y_MAX - GSL_RAW_Y_MIN);

    if (dx >= BSP_LCD_H_RES) dx = BSP_LCD_H_RES - 1;
    if (dy >= BSP_LCD_V_RES) dy = BSP_LCD_V_RES - 1;

    /* mirror_x (model config) */
    pt->x = (uint16_t)(BSP_LCD_H_RES - 1 - dx);
    pt->y = (uint16_t)dy;
    pt->pressed = true;
    return true;
}
