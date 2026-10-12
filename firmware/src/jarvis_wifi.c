/*
 * Wi-Fi for the JARVIS satellite.
 *
 * The ESP32-P4 has no radio: Wi-Fi exists only through the on-board ESP32-C6
 * attached over SDIO, driven by the ESP-Hosted stack (esp_wifi_remote).
 * With CONFIG_ESP_WIFI_REMOTE_ENABLED disabled the whole block compiles out
 * and the panel runs as a bench device (UI still works, APP shows "No Wi-Fi").
 *
 * Design notes:
 *  - connect failures are surfaced via jarvis_state_* (status line on the
 *    panel) and rate-limited in the log; the app NEVER aborts on Wi-Fi.
 *  - the disconnect handler retries forever with backoff; reason codes are
 *    translated to human text (e.g. password rejected) for the panel.
 */
#include "jarvis_wifi.h"
#include "jarvis_config.h"
#include "jarvis_state.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

static bool s_connected;
static bool s_started;

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

/* Human-readable text for the most common disconnect reasons. */
static const char *reason_text(int r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:     return "password rejected";
    case WIFI_REASON_NO_AP_FOUND:           return "AP not found";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_REJECT:          return "AP refused (2.4 GHz?)";
    case WIFI_REASON_BEACON_TIMEOUT:        return "AP dropping (signal?)";
    default:                                return "reconnecting";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static int s_streak = 0;

    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        jarvis_state_status("wifi: connecting");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        int reason = ev ? ev->reason : 0;
        s_connected = false;
        jarvis_state_conn(false, false);
        if (++s_streak <= 3 || (s_streak % 5) == 0) {
            ESP_LOGW(TAG, "disconnected (%d: %s), retrying", reason,
                     reason_text(reason));
        }
        jarvis_state_status("wifi down");
        char buf[64];
        snprintf(buf, sizeof(buf), "wifi: %s", reason_text(reason));
        jarvis_state_error(buf);
        if (reason != WIFI_REASON_AUTH_LEAVE) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR " (rssi ok, attempt #%d)",
                 IP2STR(&ev->ip_info.ip), s_streak);
        s_connected = true;
        jarvis_state_conn(true, false);
    }
}

esp_err_t jarvis_wifi_start(const char *ssid, const char *pass)
{
    esp_err_t err;

    if (!ssid || !ssid[0]) {
        jarvis_state_status("wifi not configured — console: w <ssid> <pass>");
        return ESP_ERR_INVALID_ARG;
    }

    err = jarvis_net_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(esp_netif_create_default_wifi_sta(), TAG, "netif_sta");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK) {
        /* ESP-Hosted transport problems (C6 slave missing / SDIO) land here. */
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (on-board C6 slave present?)",
                 esp_err_to_name(err));
        jarvis_state_error("wifi: C6 link failed");
        return err;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL,
                                                        NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: mode failed");
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: config failed");
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: start failed");
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "station started, joining \"%s\"", ssid);
    return ESP_OK;
}

bool jarvis_wifi_connected(void) { return s_connected; }
bool jarvis_wifi_started(void)   { return s_started; }

#else  /* !JARVIS_HAVE_WIFI */

esp_err_t jarvis_wifi_start(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    ESP_LOGE(TAG, "Wi-Fi unavailable: ESP32-P4 has no radio. Enable Esp-Hosted");
    ESP_LOGE(TAG, "(ESP32-C6 over SDIO). The UI and protocol state machine");
    ESP_LOGE(TAG, "still run for bench testing.");
    jarvis_state_conn(false, false);
    jarvis_state_status("No Wi-Fi (ESP-Hosted off)");
    return ESP_ERR_NOT_SUPPORTED;
}

bool jarvis_wifi_connected(void) { return false; }
bool jarvis_wifi_started(void)   { return false; }

#endif /* JARVIS_HAVE_WIFI */
