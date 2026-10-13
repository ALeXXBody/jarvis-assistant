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
#include <stdlib.h>
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

/* GS L36xx touch is not calibrated here: coordinates map 1:1 (vendor
 * parity) — the model-specific raw windows below are no longer needed. */

static i2c_master_dev_handle_t s_dev;

/* Calibration helpers live below; bsp_touch_init() runs them at boot. */
static void cal_load(void);

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
    cal_load();
    return ESP_OK;
}

/* ---------------- touch reads + 4-corner self calibration ------------------
 * The GSL reports panel-pixel coordinates, but each unit's axis direction is
 * only knowable in the field. Instead of hard-coding a transform, the panel
 * self-calibrates: four corner touches, a linear map raw->screen, stored in
 * NVS. All mapped reads go through j_touch_map(); bsp_touch_raw() exposes
 * the untransformed values for the calibration screen itself.               */
#include "nvs_flash.h"
#include "nvs.h"

#define CAL_NS "touch"

typedef struct {
    bool     ok;
    uint16_t rx[4], ry[4];      /* raw points at the 4 calibrated corners   */
} touch_cal_t;

static touch_cal_t s_cal;
static bool        s_cal_mode;   /* calibration collection in progress     */
static bool        s_cal_dirty;

static void cal_load(void)
{
    memset(&s_cal, 0, sizeof(s_cal));
    nvs_handle_t h;
    if (nvs_open(CAL_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_cal);
        if (nvs_get_blob(h, "cal", &s_cal, &sz) != ESP_OK) {
            s_cal.ok = false;
        }
        nvs_close(h);
    }
    /* sanity: the stored corners must be spread out, or recalibrate */
    if (s_cal.ok) {
        int dx = s_cal.rx[2] - s_cal.rx[0];
        int dy = s_cal.ry[2] - s_cal.ry[0];
        if (abs(dx) < 400 || abs(dy) < 500) {
            s_cal.ok = false;
        }
    }
}

static void cal_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CAL_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cal", &s_cal, sizeof(s_cal));
        nvs_commit(h);
        nvs_close(h);
    }
}

void bsp_touch_calibration_reset(void)
{
    s_cal.ok = false;
    cal_save();
}

void bsp_touch_calibration_enable(void)   /* UI collects 4 raw corners */
{
    s_cal_mode = true;
}

void bsp_touch_calibration_disable(void)
{
    s_cal_mode = false;
}

bool bsp_touch_calibrated(void)
{
    return s_cal.ok;
}

/* Feed one raw corner sample; returns true when all 4 are in and stored. */
bool bsp_touch_calibration_sample(int corner, uint16_t rx, uint16_t ry)
{
    if (corner < 0 || corner > 3) {
        return false;
    }
    s_cal.rx[corner] = rx;
    s_cal.ry[corner] = ry;
    /* corners: 0 TL, 1 TR, 2 BR, 3 BL — enough for a per-axis linear map */
    int dx01 = s_cal.rx[1] - s_cal.rx[0];
    int dx32 = s_cal.rx[2] - s_cal.rx[3];
    int est_x = (abs(dx01) + abs(dx32)) / 2;
    if (est_x < 400) {
        return false;
    }
    int dy03 = s_cal.ry[3] - s_cal.ry[0];
    int dy12 = s_cal.ry[2] - s_cal.ry[1];
    int est_y = (abs(dy03) + abs(dy12)) / 2;
    if (est_y < 500) {
        return false;
    }
    s_cal.ok = true;
    cal_save();
    return true;
}

/* raw -> screen, averaged from both diagonals (TL = raw[0], TR = [1],
 * BR = [2], BL = [3]); targets are screen corners with an inset margin. */
static void cal_map(uint16_t rx, uint16_t ry, uint16_t *sx, uint16_t *sy)
{
    const int M = 20;                    /* calibration target inset      */
    int w = BSP_LCD_H_RES - 1 - 2 * M;
    int h = BSP_LCD_V_RES - 1 - 2 * M;

    /* left edge average vs right edge average */
    int lxr = (s_cal.rx[0] + s_cal.rx[3]) / 2;
    int rxr = (s_cal.rx[1] + s_cal.rx[2]) / 2;
    int t_yr = (s_cal.ry[0] + s_cal.ry[1]) / 2;
    int b_yr = (s_cal.ry[2] + s_cal.ry[3]) / 2;

    int x = M, y = M;
    if (rxr != lxr) {
        x = M + ((int)rx - lxr) * w / (rxr - lxr);
    }
    if (b_yr != t_yr) {
        y = M + ((int)ry - t_yr) * h / (b_yr - t_yr);
    }
    if (x < 0)    x = 0;
    if (x > BSP_LCD_H_RES - 1) x = BSP_LCD_H_RES - 1;
    if (y < 0)    y = 0;
    if (y > BSP_LCD_V_RES - 1) y = BSP_LCD_V_RES - 1;
    *sx = (uint16_t)x;
    *sy = (uint16_t)y;
}

bool bsp_touch_raw(uint16_t *rx, uint16_t *ry)
{
    if (!rx || !ry || !s_dev) {
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
        s_no_touch++;
        return false;
    }
    s_taps++;
    *rx = (uint16_t)(((buf[7] & 0x0f) << 8) | buf[6]);
    *ry = (uint16_t)((buf[5] << 8) | buf[4]);
    return true;
}

bool bsp_touch_read(bsp_touch_point_t *pt)
{
    uint16_t rx, ry;
    if (!pt || !bsp_touch_raw(&rx, &ry)) {
        if (pt) {
            pt->pressed = false;
        }
        return false;
    }
    if (s_cal_mode || !s_cal.ok) {
        /* calibration collector (or uncalibrated board): RAW coordinates */
        pt->pressed = true;
        pt->x = rx;
        pt->y = ry;
        return true;
    }
    uint16_t sx, sy;
    cal_map(rx, ry, &sx, &sy);
    pt->pressed = true;
    pt->x = sx;
    pt->y = sy;
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
