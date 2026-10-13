/*
 * Wi-Fi bring-up.
 *
 * IMPORTANT: the ESP32-P4 has no radio. Wi-Fi on this board is provided by the
 * on-board ESP32-C6 through ESP-Hosted (SDIO). When ESP-Hosted is enabled
 * (CONFIG_ESP_WIFI_REMOTE_ENABLED) the standard esp_wifi_* API is routed to
 * the C6 transparently, so the code below is the ordinary station flow.
 *
 * Until the C6 slave firmware is flashed and ESP-Hosted is enabled, this
 * compiles to a clear "Wi-Fi unavailable" path so the rest of the firmware
 * can be built and bench-tested. See docs/BRINGUP.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];        /* zero-terminated */
    int8_t rssi;          /* dBm             */
} jarvis_wifi_ap_t;

/* Brings up esp_netif + the default event loop. Idempotent and safe to call
 * when Wi-Fi is unavailable — the TCP/IP stack the WebSocket client needs
 * still has to exist. */
esp_err_t jarvis_net_init(void);

/* Starts the station. Returns once the driver is up (not necessarily
 * associated). Association progress is reported through the state module. */
esp_err_t jarvis_wifi_start(const char *ssid, const char *pass);

bool jarvis_wifi_connected(void);
bool jarvis_wifi_started(void);   /* esp_wifi_start() succeeded (not link up) */

/* Blocking neighbour scan; caller provides storage. Up to `max` APs are
 * returned, strongest first. Needs the radio started: jarvis_wifi_start("",
 * NULL) is enough — station mode, no joining. */
esp_err_t jarvis_wifi_scan(jarvis_wifi_ap_t *out, size_t *cnt, size_t max);
