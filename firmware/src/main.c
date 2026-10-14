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
 *
 * Debug aid: 10 s after boot a "boot report" line re-states the outcome of
 * every bring-up step — because the early-boot log is invisible when the
 * serial console / browser page only attaches after the panel is running
 * (main.c, boot_recap_task).
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
#include "jarvis_ota.h"

static const char *TAG = "jarvis";

/* Outcome of each bring-up step; re-reported by the boot report. */
static struct {
    bool display, touch, led, battery, ui, audio, ws, wifi, wake;
    bool psram, ota;
} s_boot;

/* Log-and-continue bring-up: log failures, record outcome. */
static void try_log(const char *what, esp_err_t e, bool *ok)
{
    *ok = (e == ESP_OK);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s (continuing)", what, esp_err_to_name(e));
    }
}

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
    s_boot.psram = (esp_psram_get_size() >= 1024 * 1024);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " J.A.R.V.I.S. satellite  fw %s", jarvis_ota_running_version());
    ESP_LOGI(TAG, " chip=%d cores=%d rev=%d.%d", info.model, info.cores,
             info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, " flash=%luMB psram=%luKB",
             (unsigned long)(flash / (1024 * 1024)),
             (unsigned long)(esp_psram_get_size() / 1024));
    ESP_LOGI(TAG, "==================================================");
}

/* Re-states bring-up results ~10 s after boot, when an attached console or
 * browser page can actually see them (the boot-time lines are unrecoverable
 * otherwise). Call sites that matter to the black-screen diagnosis are
 * explicit; this runs once and exits. */
static void boot_recap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI(TAG, "---------------- boot report ----------------");
    ESP_LOGI(TAG, " fw=%s  psram=%s  ota=%s",
             jarvis_ota_running_version(),
             s_boot.psram ? "OK" : "MISSING!",
             s_boot.ota ? "OK" : "FAILED");
    ESP_LOGI(TAG, " display=%s  touch=%s  led=%s  battery=%s",
             s_boot.display ? "OK" : "FAILED",
             s_boot.touch ? "OK" : "FAILED",
             s_boot.led ? "OK" : "FAILED",
             s_boot.battery ? "OK" : "FAILED");
    ESP_LOGI(TAG, " ui=%s  audio=%s  wake=%s",
             s_boot.ui ? "OK" : "FAILED",
             s_boot.audio ? "OK" : "FAILED",
             s_boot.wake ? "OK" : "FAILED");
    ESP_LOGI(TAG, " ws=%s  wifi=%s",
             jarvis_ws_connected() ? "connected" : "down",
             s_boot.wifi ? "started" : "disabled");
    ESP_LOGI(TAG, "--------------- end boot report ---------------");
    vTaskDelete(NULL);
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
    try_log("ota", jarvis_ota_init(), &s_boot.ota);
    const jarvis_settings_t *cfg = jarvis_settings();

    /* --- network stack (needed by the WS client even without Wi-Fi) ------- */
    ESP_ERROR_CHECK(jarvis_net_init());

    /* --- BSP bring-up (non-fatal) ---------------------------------------- */
    try_log("display", bsp_display_init(), &s_boot.display);
    try_log("touch",   bsp_touch_init(),   &s_boot.touch);
    try_log("led",     bsp_led_init(),     &s_boot.led);
    try_log("battery", bsp_battery_init(), &s_boot.battery);

    /* --- renderer + audio + turn engine ---------------------------------- */
    try_log("ui",    jarvis_ui_init(), &s_boot.ui);
    try_log("audio", jarvis_audio_init(), &s_boot.audio);
    jarvis_turn_init();

    /* --- connect to the brain -------------------------------------------- */
    try_log("ws", jarvis_ws_start(cfg->server_url), &s_boot.ws);

    /* --- Wi-Fi (ESP-Hosted to the on-board C6; may be disabled) ---------- */
    try_log("wifi", jarvis_wifi_start(cfg->wifi_ssid, cfg->wifi_pass), &s_boot.wifi);

    /* --- triggers (touch tap + serial console) --------------------------- */
    try_log("wake", jarvis_wake_init(), &s_boot.wake);

    /* --- housekeeping ----------------------------------------------------- */
    if (xTaskCreatePinnedToCore(battery_task, "jarvis_bat", 3072, NULL, 3,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "battery task not started");
    }
    if (xTaskCreatePinnedToCore(boot_recap_task, "jarvis_recap", 4096, NULL, 2,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "boot report task not started");
    }

    ESP_LOGI(TAG, "boot complete: state=%s server=%s",
             jarvis_state_name(jarvis_state_get()), cfg->server_url);

    /* --- firmware: confirm this image is good, then look for an update ---- */
    /* With bootloader rollback enabled a fresh OTA image boots in "pending
     * verify"; reaching this point means bring-up succeeded, so cancel the
     * rollback. Then run one background update check (no-op if no URL). */
    jarvis_ota_mark_valid();
    jarvis_ota_autocheck_start();

    /* app_main may return; all work runs in the tasks above. */
}
