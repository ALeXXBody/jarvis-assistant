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
#include "esp_err.h"

/* Brings up esp_netif + the default event loop. Idempotent and safe to call
 * when Wi-Fi is unavailable — the TCP/IP stack the WebSocket client needs
 * still has to exist. */
esp_err_t jarvis_net_init(void);

/* Starts the station. Returns once the driver is up (not necessarily
 * associated). Association progress is reported through the state module. */
esp_err_t jarvis_wifi_start(const char *ssid, const char *pass);

bool jarvis_wifi_connected(void);
