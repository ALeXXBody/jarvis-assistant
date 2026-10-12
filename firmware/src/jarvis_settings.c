#include "jarvis_settings.h"
#include "jarvis_config.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NVS_NS = "jarvis";

static jarvis_settings_t s_cfg;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap, const char *dflt)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, dflt, cap);
    } else {
        dst[cap - 1] = '\0';
    }
}

esp_err_t jarvis_settings_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no NVS namespace yet (%s); using defaults", esp_err_to_name(err));
        strlcpy(s_cfg.wifi_ssid, JARVIS_DEFAULT_WIFI_SSID, sizeof(s_cfg.wifi_ssid));
        strlcpy(s_cfg.wifi_pass, JARVIS_DEFAULT_WIFI_PASS, sizeof(s_cfg.wifi_pass));
        strlcpy(s_cfg.server_url, JARVIS_DEFAULT_SERVER, sizeof(s_cfg.server_url));
        strlcpy(s_cfg.ota_url, JARVIS_DEFAULT_OTA_URL, sizeof(s_cfg.ota_url));
        strlcpy(s_cfg.conversation, JARVIS_DEFAULT_CONVERSATION, sizeof(s_cfg.conversation));
        s_cfg.volume = 75;
        return ESP_OK;
    }

    load_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), JARVIS_DEFAULT_WIFI_SSID);
    load_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), JARVIS_DEFAULT_WIFI_PASS);
    load_str(h, "server_url", s_cfg.server_url, sizeof(s_cfg.server_url), JARVIS_DEFAULT_SERVER);
    load_str(h, "ota_url", s_cfg.ota_url, sizeof(s_cfg.ota_url), JARVIS_DEFAULT_OTA_URL);
    load_str(h, "conversation", s_cfg.conversation, sizeof(s_cfg.conversation), JARVIS_DEFAULT_CONVERSATION);
    int32_t vol = 75;
    if (nvs_get_i32(h, "volume", &vol) != ESP_OK) {
        vol = 75;
    }
    s_cfg.volume = (int)vol;
    nvs_close(h);

    ESP_LOGI(TAG, "settings: ssid='%s' server='%s' conv='%s' vol=%d",
             s_cfg.wifi_ssid, s_cfg.server_url, s_cfg.conversation, s_cfg.volume);
    ESP_LOGI(TAG, "settings: ota_url='%s'", s_cfg.ota_url);
    return ESP_OK;
}

const jarvis_settings_t *jarvis_settings(void)
{
    return &s_cfg;
}

esp_err_t jarvis_settings_save(const jarvis_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", s->wifi_ssid);
    nvs_set_str(h, "wifi_pass", s->wifi_pass);
    nvs_set_str(h, "server_url", s->server_url);
    nvs_set_str(h, "ota_url", s->ota_url);
    nvs_set_str(h, "conversation", s->conversation);
    nvs_set_i32(h, "volume", s->volume);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = *s;
    }
    return err;
}

esp_err_t jarvis_settings_set_wifi(const char *ssid, const char *pass)
{
    jarvis_settings_t s = s_cfg;
    strlcpy(s.wifi_ssid, ssid ? ssid : "", sizeof(s.wifi_ssid));
    strlcpy(s.wifi_pass, pass ? pass : "", sizeof(s.wifi_pass));
    return jarvis_settings_save(&s);
}

esp_err_t jarvis_settings_set_server(const char *url, const char *conversation)
{
    jarvis_settings_t s = s_cfg;
    if (url) {
        strlcpy(s.server_url, url, sizeof(s.server_url));
    }
    if (conversation) {
        strlcpy(s.conversation, conversation, sizeof(s.conversation));
    }
    return jarvis_settings_save(&s);
}

esp_err_t jarvis_settings_set_ota_url(const char *url)
{
    jarvis_settings_t s = s_cfg;
    if (url) {
        strlcpy(s.ota_url, url, sizeof(s.ota_url));
    }
    return jarvis_settings_save(&s);
}

esp_err_t jarvis_settings_set_volume(int volume)
{
    jarvis_settings_t s = s_cfg;
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    s.volume = volume;
    return jarvis_settings_save(&s);
}
