/* Runtime settings backed by NVS (namespace "jarvis"). */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char server_url[160];
    char ota_url[200];    /* release base URL for OTA; "" disables updates */
    char conversation[48];
    int  volume;          /* 0..100 */
} jarvis_settings_t;

/* Loads settings from NVS, applying compile-time defaults for missing keys. */
esp_err_t jarvis_settings_init(void);

/* Returns a pointer to the live settings (read-only, never NULL). */
const jarvis_settings_t *jarvis_settings(void);

/* Persists the given settings to NVS and updates the live copy. */
esp_err_t jarvis_settings_save(const jarvis_settings_t *s);

/* Convenience setters (persist immediately). */
esp_err_t jarvis_settings_set_wifi(const char *ssid, const char *pass);
esp_err_t jarvis_settings_set_server(const char *url, const char *conversation);
esp_err_t jarvis_settings_set_ota_url(const char *url);
esp_err_t jarvis_settings_set_volume(int volume);
