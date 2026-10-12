/*
 * ES8311 audio codec on the GUITION JC8012P4A1C (ESP32-P4).
 *
 * Wiring (I2S0, full duplex, master):
 *   MCLK GPIO13, BCLK GPIO12, LRCLK(WS) GPIO10, DOUT(->codec) GPIO9,
 *   DIN(<-mic/codec) GPIO11, PA enable GPIO20.
 *
 * The ES8311 is controlled over the shared I2C bus (see bsp_guition.c) and the
 * data path goes through esp_codec_dev, which owns the ESP-IDF std I2S driver:
 * we create one full-duplex channel pair on I2S0 and let esp_codec_dev_open()
 * program the codec + (re)configure/enable the I2S channels for us.
 */
#include "bsp_guition.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "bsp_audio";

#define BSP_AUDIO_SAMPLE_RATE   16000
#define BSP_AUDIO_MCLK_MULT     256
#define BSP_AUDIO_DEFAULT_VOL   70
#define BSP_AUDIO_I2S_DMA_DESC  6

static i2s_chan_handle_t        s_tx;
static i2s_chan_handle_t        s_rx;
static esp_codec_dev_handle_t   s_dev;
static int                      s_volume = BSP_AUDIO_DEFAULT_VOL;
static bool                     s_ready;

esp_err_t bsp_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init");

    /* --- I2S0 full duplex -------------------------------------------------- */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)JARVIS_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = BSP_AUDIO_I2S_DMA_DESC;
    chan_cfg.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_MCLK_GPIO,
            .bclk = BSP_AUDIO_BCLK_GPIO,
            .ws = BSP_AUDIO_LRCLK_GPIO,
            .dout = BSP_AUDIO_DOUT_GPIO,
            .din = BSP_AUDIO_DIN_GPIO,
            .invert_flags = { 0 },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = BSP_AUDIO_MCLK_MULT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "rx init");

    /* --- codec control + data interfaces ---------------------------------- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_MASTER_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,   /* 0x30 (8-bit) -> 0x18 on the bus */
        .bus_handle = bsp_i2c_bus(),
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = JARVIS_AUDIO_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_AUDIO_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,           /* P4 is the I2S master */
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
            .pa_gain = 0.0f,
        },
        .no_dac_ref = false,
        .mclk_div = BSP_AUDIO_MCLK_MULT,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = BSP_AUDIO_SAMPLE_RATE,
        .mclk_multiple = BSP_AUDIO_MCLK_MULT,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_dev, &fs), TAG, "codec open");
    esp_codec_dev_set_out_vol(s_dev, s_volume);

    s_ready = true;
    ESP_LOGI(TAG, "ES8311 up @ %d Hz mono, vol %d", BSP_AUDIO_SAMPLE_RATE, s_volume);
    return ESP_OK;
}

int bsp_audio_record(uint8_t *buf, size_t max, uint32_t timeout_ms)
{
    (void)timeout_ms;   /* esp_codec_dev_read blocks on the I2S DMA queue */
    if (!s_ready || !buf || max == 0) {
        return 0;
    }
    if (esp_codec_dev_read(s_dev, buf, (int)max) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    return (int)max;    /* fixed-size frames: a successful read filled |max| */
}

esp_err_t bsp_audio_play(const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_ready || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_write(s_dev, (void *)buf, (int)len) == ESP_CODEC_DEV_OK)
               ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (s_ready) {
        esp_codec_dev_set_out_vol(s_dev, s_volume);
    }
    return ESP_OK;
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_set_out_mute(s_dev, mute) == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}
