/*
 * Battery monitor.
 *
 * A resistor divider (ratio 1.68) scales the battery to ADC2 channel 3
 * (GPIO52 on the ESP32-P4). The P4 has no internal radio so ADC2 is free to
 * use. Voltage is reported at the battery terminal (post-divider * 1.68).
 */
#include "bsp_guition.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "bsp_sys";

#define BAT_ADC_UNIT      ADC_UNIT_2
#define BAT_ADC_CHANNEL   ADC_CHANNEL_3      /* GPIO52 */
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH  ADC_BITWIDTH_DEFAULT

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_cali_ok;
static bool                      s_ready;

esp_err_t bsp_battery_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc new unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg),
                        TAG, "adc config channel");

    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "no ADC calibration scheme; using raw scaling");
    }

    s_ready = true;
    ESP_LOGI(TAG, "battery ADC ready (GPIO%d, /%.2f)",
             BSP_BATTERY_ADC_GPIO, (double)BSP_BATTERY_DIVIDER);
    return ESP_OK;
}

float bsp_battery_voltage(void)
{
    if (!s_ready) {
        return 0.0f;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0.0f;
    }

    int mv = 0;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return 0.0f;
        }
    } else {
        mv = raw * 3300 / 4095;   /* 12-bit, ~3.3 V full scale */
    }

    return (mv / 1000.0f) * BSP_BATTERY_DIVIDER;
}
