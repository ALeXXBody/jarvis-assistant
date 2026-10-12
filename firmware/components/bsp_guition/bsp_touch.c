/*
 * GSL3680 capacitive touch controller (I2C 0x40) on the GUITION JC8012P4A1C.
 *
 * The GSL3670/GSL3680 has no usable on-chip touch firmware: after every reset
 * the host must load a firmware blob before the chip reports touches.
 *
 * The bring-up sequence below mirrors the VENDOR driver for this exact board
 * (JC8012P4A1C_I_W_Y NewPanel example, esp_lcd_gsl3680.c), including:
 *   - the INT pin driven LOW across the first reset (pin-latch phase),
 *   - the merged chip/scan engine register bounces with THEIR delays,
 *   - the firmware write + the 0xB0 == 5A5A5A5A verify with retries.
 *
 * The register protocol follows the vendor flow:
 *
 *   vendor_clear_reg(): 0xe0=0x88 | 0x88=0x01 | 0xe4=0x04 | 0xe0=0x00
 *   vendor_reset():     GPIO RST low 20 ms, high 20 ms, then
 *                       0xe0=0x88 | 0xe4=0x04 | 0xbc=00 00 00 00
 *   load fw             (our blob, 132-byte blocks — engine remains armed
 *                        using the same blob that worked on v0.1.3)
 *   startup:            0xe0=0x00
 *   verify:             read 0xb0 == 5a 5a 5a 5a (worried → full retry)
 *
 * Report frame (read from reg 0x80, 44 bytes): fingers same mapping as the
 * vendor read (touch_data[0] = finger count, first finger at offset 4).
 *
 * Raw -> panel mapping for this model (ESPHome): swap_xy, mirror_x,
 * mirror_y=false, raw x/y spans 20..880 / 20..1648.
 */
#include "bsp_guition.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

/* Firmware blob (legacy block format, kept for reference):
 * components/bsp_guition/gsl3680_fw.c — the vendor-record loader below
 * supersedes it. */
extern const uint8_t gsl3680_fw[];
extern const size_t  gsl3680_fw_blocks;

/* Vendor firmware records, verbatim from the board's own example driver
 * (esp_lcd_gsl3680.h): {register offset, 32-bit LE value}. */
extern const struct gsl_fw_rec {
    uint8_t off;
    uint32_t val;
} gsl_vfw[];
#define GSL_VFW_CNT 4587

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

/* Diagnostic counters (console `touch` command). */
static uint32_t s_taps, s_no_touch, s_i2c_err;
static int      s_boot_attempts;      /* bring-up retries for the diag   */
static bool     s_boot_verified;      /* last 0xb0 == 5A x4 result       */

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

static esp_err_t gsl_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

/* ---- vendor bring-up steps (esp_lcd_gsl3680.c parity) -------------------- */

/* Vendor clear_reg — note 0xe0=0x88 FIRST (this is what our previous
 * build missed: only 0x88=0x01 had been written). */
static esp_err_t v_clear_reg(void)
{
    ESP_RETURN_ON_ERROR(gsl_write8(0xe0, 0x88), TAG, "e0=88");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gsl_write8(0x88, 0x01), TAG, "88=01");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl_write8(0xe4, 0x04), TAG, "e4=04");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl_write8(0xe0, 0x00), TAG, "e0=00");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

/* Vendor reset — LONG GPIO pulses (20 ms) + the three register writes. */
static esp_err_t v_reset(void)
{
    gpio_set_level(BSP_TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BSP_TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(gsl_write8(0xe0, 0x88), TAG, "e0=88");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gsl_write8(0xe4, 0x04), TAG, "e4=04");
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint8_t zeros[4] = { 0, 0, 0, 0 };
    ESP_RETURN_ON_ERROR(gsl_write(0xbc, zeros, sizeof(zeros)), TAG, "bc");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* Verbatim vendor load: one I2C write per record, exactly as the vendor
 * driver does (1 byte for the 0xf0 marker, 4-byte LE elsewhere). */
static esp_err_t v_load_fw(void)
{
    for (size_t i = 0; i < GSL_VFW_CNT; i++) {
        const uint8_t off = gsl_vfw[i].off;
        if (off == 0xf0) {
            uint8_t v = (uint8_t)(gsl_vfw[i].val & 0xff);
            ESP_RETURN_ON_ERROR(gsl_write(off, &v, 1), TAG, "fw rec %u", i);
        } else {
            uint8_t w[4] = {
                (uint8_t)(gsl_vfw[i].val & 0xff),
                (uint8_t)((gsl_vfw[i].val >> 8) & 0xff),
                (uint8_t)((gsl_vfw[i].val >> 16) & 0xff),
                (uint8_t)((gsl_vfw[i].val >> 24) & 0xff),
            };
            ESP_RETURN_ON_ERROR(gsl_write(off, w, 4), TAG, "fw rec %u", i);
        }
    }
    return ESP_OK;
}

static esp_err_t v_startup(void)
{
    ESP_RETURN_ON_ERROR(gsl_write8(0xe0, 0x00), TAG, "e0=00");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* Vendor verify: 0xb0 must read back 0x5a 5a 5a 5a after the load. */
static esp_err_t v_verify(void)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    ESP_RETURN_ON_ERROR(gsl_read(0xb0, b, sizeof(b)), TAG, "read 0xb0");
    s_boot_verified = (b[0] == 0x5a && b[1] == 0x5a && b[2] == 0x5a && b[3] == 0x5a);
    ESP_LOGW(TAG, "0xb0 verify: %02x %02x %02x %02x → %s",
             b[3], b[2], b[1], b[0], s_boot_verified ? "OK" : "FAILED");
    return s_boot_verified ? ESP_OK : ESP_FAIL;
}

static esp_err_t v_bring_up_once(void)
{
    ESP_RETURN_ON_ERROR(v_clear_reg(), TAG, "clear_reg");
    ESP_RETURN_ON_ERROR(v_reset(),     TAG, "reset");
    ESP_RETURN_ON_ERROR(v_load_fw(),   TAG, "load_fw");
    ESP_RETURN_ON_ERROR(v_startup(),   TAG, "startup");
    ESP_RETURN_ON_ERROR(v_reset(),     TAG, "reset2");
    ESP_RETURN_ON_ERROR(v_startup(),   TAG, "startup2");
    vTaskDelay(pdMS_TO_TICKS(30));     /* vendor read-delay before verify */
    return v_verify();
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

    /* RST + INT outputs; INT held LOW through the reset latch (pin phase
     * from the vendor driver: address latch + wake-up state).            */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_RST_GPIO) | (1ULL << BSP_TOUCH_INT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "rst/int gpio");

    gpio_set_level(BSP_TOUCH_INT_GPIO, 0);
    gpio_set_level(BSP_TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Full vendor bring-up, up to three verified attempts.                */
    esp_err_t err = ESP_FAIL;
    for (s_boot_attempts = 1; s_boot_attempts <= 3; s_boot_attempts++) {
        err = v_bring_up_once();
        if (err == ESP_OK) {
            break;
        }
        if (s_boot_attempts < 3) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* INT back to input once the chip is up (kept neutral thereafter).    */
    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << BSP_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "int input");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch bring-up failed after %d attempts", s_boot_attempts);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- touch reads ---------------------------------------------------------- */

bool bsp_touch_read(bsp_touch_point_t *pt)
{
    if (!pt || !s_dev) {
        return false;
    }

    uint8_t reg = GSL_REG_DATA;
    uint8_t buf[GSL_DATA_LEN];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 100) != ESP_OK) {
        s_i2c_err++;
        return false;
    }

    uint8_t fingers = buf[0] > GSL_MAX_FINGERS ? GSL_MAX_FINGERS : buf[0];
    if (fingers == 0) {
        pt->pressed = false;
        s_no_touch++;
        return false;
    }
    s_taps++;

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

/* On-demand probe from the serial console (`touch` command): counters plus
 * a raw 44-byte data capture, plus the current 0xf0 and 0xb0 windows so a
 * stuck chip shows up as data instead of silence.                   */
void bsp_touch_diag(void)
{
    if (!s_dev) {
        ESP_LOGW(TAG, "diag: touch device not initialised");
        return;
    }
    uint8_t reg = GSL_REG_DATA;
    uint8_t buf[GSL_DATA_LEN];
    esp_err_t e = gsl_read(reg, buf, sizeof(buf));
    ESP_LOGW(TAG, "diag: read=%s taps=%u no_touch=%u i2c_err=%u "
                  "boot tries=%d 0xb0=%s",
             esp_err_to_name(e), s_taps, s_no_touch, s_i2c_err,
             s_boot_attempts, s_boot_verified ? "5a-ok" : "not-ok");
    if (e == ESP_OK) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, sizeof(buf), ESP_LOG_WARN);
    }
    uint8_t f0[4] = { 0, 0, 0, 0 };
    if (gsl_read(0xf0, f0, sizeof(f0)) == ESP_OK) {
        ESP_LOGW(TAG, "0xf0 ping: %02x %02x %02x %02x", f0[0], f0[1], f0[2], f0[3]);
    }
}
