/*
 * JARVIS satellite — application entry point.
 *
 * Boot order matters:
 *   NVS + settings + state  ->  netif/event loop (TCP/IP for the WS client)
 *   ->  BSP (display, touch, audio, LED, battery)
 *   ->  UI task, audio tasks, turn engine
 *   ->  WebSocket client to the brain (assistant host)  ->  Wi-Fi/ESP-Hosted
 *   ->  trigger tasks (touch tap + serial console)
 *
 * Peripheral bring-up failures are logged, not fatal: on a bench board with a
 * panel still warming up we still want the serial console and WS link alive.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_guition.h"
#include "jarvis_config.h"
#include "jarvis_settings.h"
#include "jarvis_state.h"
#include "jarvis_ui.h"
#include "jarvis_audio.h"
#include "jarvis_turn.h"
#include "jarvis_ws.h"
#include "jarvis_wifi.h"
#include "jarvis_wake.h"

#ifndef JARVIS_FW_VERSION
#define JARVIS_FW_VERSION "dev"
#endif

static const char *TAG = "jarvis";

/* Log-and-continue helper for peripheral bring-up. */
#define TRY(what, expr)                                                   \
    do {                                                                  \
        esp_err_t _e = (expr);                                            \
        if (_e != ESP_OK) {                                               \
            ESP_LOGW(TAG, "%s failed: %s (continuing)", (what),           \
                     esp_err_to_name(_e));                                \
        }                                                                 \
    } while (0)

static int battery_pct_from_volts(float v)
{
    /* Simple Li-ion curve anchor points (resting, rough). */
    if (v <= 3.30f) return 0;
    if (v >= 4.20f) return 100;
    return (int)((v - 3.30f) * 100.0f / (4.20f - 3.30f));
}

static void battery_task(void *arg)
{
    (void)arg;
    while (true) {
        float v = bsp_battery_voltage();
        if (v > 0.1f) {
            jarvis_state_set_battery(battery_pct_from_volts(v));
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " J.A.R.V.I.S. satellite  fw %s", JARVIS_FW_VERSION);
    ESP_LOGI(TAG, " chip=%d cores=%d rev=%d.%d", info.model, info.cores,
             info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, " flash=%luMB psram=%luKB",
             (unsigned long)(flash / (1024 * 1024)),
             (unsigned long)(esp_psram_get_size() / 1024));
    ESP_LOGI(TAG, "==================================================");
}

void app_main(void)
{
    /* --- NVS (required) --------------------------------------------------- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    banner();

    ESP_ERROR_CHECK(jarvis_settings_init());
    jarvis_state_init();
    const jarvis_settings_t *cfg = jarvis_settings();

    /* --- network stack (needed by the WS client even without Wi-Fi) ------- */
    ESP_ERROR_CHECK(jarvis_net_init());

    /* --- BSP bring-up (non-fatal) ---------------------------------------- */
    TRY("display", bsp_display_init());
    TRY("touch",   bsp_touch_init());
    TRY("led",     bsp_led_init());
    TRY("battery", bsp_battery_init());

    /* --- renderer + audio + turn engine ---------------------------------- */
    TRY("ui",    jarvis_ui_init());
    TRY("audio", jarvis_audio_init());
    jarvis_turn_init();

    /* --- connect to the brain -------------------------------------------- */
    TRY("ws", jarvis_ws_start(cfg->server_url));

    /* --- Wi-Fi (ESP-Hosted to the on-board C6; may be disabled) ---------- */
    TRY("wifi", jarvis_wifi_start(cfg->wifi_ssid, cfg->wifi_pass));

    /* --- triggers (touch tap + serial console) --------------------------- */
    TRY("wake", jarvis_wake_init());

    /* --- housekeeping ----------------------------------------------------- */
    if (xTaskCreatePinnedToCore(battery_task, "jarvis_bat", 3072, NULL, 3,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "battery task not started");
    }

    ESP_LOGI(TAG, "boot complete: state=%s server=%s",
             jarvis_state_name(jarvis_state_get()), cfg->server_url);

    /* app_main may return; all work runs in the tasks above. */
}
