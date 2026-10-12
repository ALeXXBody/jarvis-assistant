#include "jarvis_wifi.h"
#include "jarvis_config.h"
#include "jarvis_state.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

static bool s_connected;

esp_err_t jarvis_net_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

#if JARVIS_HAVE_WIFI

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        jarvis_state_conn(false, false);
        ESP_LOGW(TAG, "disconnected, retrying in 3 s");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        jarvis_state_conn(true, false);
    }
}

esp_err_t jarvis_wifi_start(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        ESP_LOGW(TAG, "no SSID configured");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(jarvis_net_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "station started, connecting to \"%s\"", ssid);
    return ESP_OK;
}

bool jarvis_wifi_connected(void) { return s_connected; }

#else  /* !JARVIS_HAVE_WIFI */

esp_err_t jarvis_wifi_start(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    ESP_LOGE(TAG, "Wi-Fi unavailable: ESP32-P4 has no radio. Enable ESP-Hosted");
    ESP_LOGE(TAG, "(ESP32-C6 over SDIO) — see docs/BRINGUP.md. The UI and");
    ESP_LOGE(TAG, "protocol state machine still run for bench testing.");
    jarvis_state_conn(false, false);
    jarvis_state_status("No Wi-Fi (ESP-Hosted off)");
    return ESP_ERR_NOT_SUPPORTED;
}

bool jarvis_wifi_connected(void) { return false; }

#endif
