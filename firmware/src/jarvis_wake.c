#include "jarvis_wake.h"
#include "jarvis_config.h"
#include "jarvis_turn.h"
#include "jarvis_settings.h"
#include "jarvis_ota.h"
#include "jarvis_ws.h"
#include "jarvis_wifi.h"
#include "jarvis_state.h"
#include "jarvis_ui.h"
#include "bsp_guition.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wake";

/* --- touch: tap the screen to toggle the turn ---------------------------- */

static void touch_task(void *arg)
{
    (void)arg;
    bool was = false;
    TickType_t last = 0;

    while (true) {
        bsp_touch_point_t pt;
        bool now = bsp_touch_read(&pt);
        TickType_t t = xTaskGetTickCount();

        if (now && !was && (t - last) > pdMS_TO_TICKS(JARVIS_TAP_DEBOUNCE_MS)) {
            last = t;
            ESP_LOGI(TAG, "tap at %u,%u (raw %u,%u)", pt.x, pt.y, pt.raw_x, pt.raw_y);
            /* the UI owns all on-panel interaction: settings gear, settings
             * screen, or turn trigger — nothing needs a desktop            */
            jarvis_ui_tap(pt.x, pt.y);
        }
        was = now;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

/* --- serial console ------------------------------------------------------ */

/* --- serial console ------------------------------------------------------ */
/*
 * Line-oriented command console (each command ends with a newline):
 *
 *   l / s / x / a / d     listen stop abort allow deny   (turn control)
 *   r                     reconnect the WebSocket to the brain
 *   u                     check for firmware updates now
 *   + / -                 volume up / down (5 %)
 *   w <ssid> <pass>       save and join Wi-Fi (persisted in NVS)
 *   srv <ws://host:port/ws> [conversation]
 *                         set the brain URL (persisted) and reconnect
 *   cfg                   show current settings (not secrets)
 *   state                 print the runtime state snapshot
 *
 * Secrets (Wi-Fi password) live only on the panel: they are typed over
 * serial/bus, stored in NVS and never sent to logs or to the remote side.
 */

static void cfg_show(void)
{
    const jarvis_settings_t *c = jarvis_settings();
    ESP_LOGI(TAG, "cfg: state=%s", jarvis_state_name(jarvis_state_get()));
    ESP_LOGI(TAG, "cfg: server=%s conv=%s ota=%s",
             c->server_url, c->conversation,
             c->ota_url[0] ? c->ota_url : "(none)");
    ESP_LOGI(TAG, "cfg: wifi ssid=\"%s\" wifi=%s ws=%s volume=%d",
             c->wifi_ssid,
             jarvis_wifi_started() ? (jarvis_wifi_connected() ? "up" : "joining") : "down",
             jarvis_ws_connected() ? "up" : "down",
             c->volume);
}

static void cfg_state(void)
{
    jarvis_snapshot_t s;
    jarvis_state_copy(&s);
    ESP_LOGI(TAG, "state: %s wifi=%d ws=%d battery=%d status=\"%s\" last=%s",
             jarvis_state_name(s.state), s.wifi_connected, s.ws_connected,
             s.battery_pct, s.status,
             s.last_total_seconds > 0.1f ? "ok" : "n/a");
}

static void console_exec(char *line)
{
    /* tokenise in place */
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd || !cmd[0]) {
        return;
    }

    if (cmd[1] == '\0') {
        switch (cmd[0]) {
        case 'l': jarvis_turn_start_listening(); return;
        case 's': jarvis_turn_stop_listening();  return;
        case 'x': jarvis_turn_abort();           return;
        case 'a': jarvis_turn_approval(true);    return;
        case 'd': jarvis_turn_approval(false);   return;
        case 'r': jarvis_ws_start(jarvis_settings()->server_url); return;
        case 'u':
            ESP_LOGI(TAG, "checking for firmware updates...");
            if (!jarvis_ota_check_async()) {
                ESP_LOGW(TAG, "update check unavailable (busy / no network)");
            }
            return;
        case '+': {
            const jarvis_settings_t *c = jarvis_settings();
            jarvis_settings_set_volume(c->volume + 5);
            bsp_audio_set_volume(jarvis_settings()->volume);
            return;
        }
        case '-': {
            const jarvis_settings_t *c = jarvis_settings();
            jarvis_settings_set_volume(c->volume - 5);
            bsp_audio_set_volume(jarvis_settings()->volume);
            return;
        }
        default:
            ESP_LOGW(TAG, "unknown command '%c' — type 'help'", cmd[0]);
            return;
        }
    }

    if (!strcmp(cmd, "w") || !strcmp(cmd, "wifi")) {
        char *ssid = strtok_r(NULL, " \t", &save);
        char *pass = strtok_r(NULL, " \t", &save);
        if (!ssid) {
            ESP_LOGW(TAG, "usage: w <ssid> <pass>");
            return;
        }
        esp_err_t e = jarvis_settings_set_wifi(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "wifi set \"%s\": %s", ssid, esp_err_to_name(e));
        if (e == ESP_OK) {
            jarvis_turn_abort();  /* reset any stuck turn state */
            e = jarvis_wifi_start(ssid, pass ? pass : "");
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "wifi_start: %s", esp_err_to_name(e));
            }
        }
        return;
    }

    if (!strcmp(cmd, "srv")) {
        const jarvis_settings_t *c = jarvis_settings();
        char *url = strtok_r(NULL, " \t", &save);
        char *conv = strtok_r(NULL, " \t", &save);
        esp_err_t e = jarvis_settings_set_server(url ? url : c->server_url,
                                                 conv ? conv : c->conversation);
        ESP_LOGI(TAG, "srv set: %s", esp_err_to_name(e));
        if (e == ESP_OK && url) {
            jarvis_ws_start(jarvis_settings()->server_url);
        }
        return;
    }

    if (!strcmp(cmd, "touch")) {
        bsp_touch_diag();
        return;
    }

    if (!strcmp(cmd, "cfg")) {
        cfg_show();
        return;
    }

    if (!strcmp(cmd, "state")) {
        cfg_state();
        return;
    }

    if (!strcmp(cmd, "help")) {
        ESP_LOGI(TAG, "console: l/s/x/a/d = turn ctrl | r reconnect | u update"
                      " | +/- vol | w <ssid> <pass> | srv <url> [conv]"
                      " | cfg | state | touch | help");
        return;
    }

    ESP_LOGW(TAG, "unknown command '%s' — type 'help'", cmd);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[160];
    size_t n = 0;
    TickType_t last_rx = 0;

    ESP_LOGI(TAG, "console ready — type 'help' for commands");
    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            /* flush a pending line if input went silent (senders that do
             * not terminate with a newline, e.g. the browser send box) */
            if (n && last_rx &&
                (xTaskGetTickCount() - last_rx) > pdMS_TO_TICKS(600)) {
                line[n] = '\0';
                ESP_LOGI(TAG, "cmd: %s", line);
                console_exec(line);
                n = 0;
                last_rx = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_rx = xTaskGetTickCount();
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || ch == ']') {   /* ] = Enter on some serial bridges */
            if (n > 0) {
                line[n] = '\0';
                ESP_LOGI(TAG, "cmd: %s", line);
                console_exec(line);
                n = 0;
                last_rx = 0;
            }
            continue;
        }
        if (n < sizeof(line) - 1) {
            line[n++] = (char)ch;
        }
    }
}

/* --- wake word hook ------------------------------------------------------ */

#ifdef JARVIS_ENABLE_WAKENET
/* A WakeNet task would live here: esp_sr feed() + model detection. */
bool jarvis_wake_feed(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples;
    return false;
}
#else
bool jarvis_wake_feed(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples;
    return false;
}
#endif

esp_err_t jarvis_wake_init(void)
{
    if (xTaskCreatePinnedToCore(touch_task, "jarvis_touch", 4096, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }
    /* Console reader: harmless (and just blocks) if the console has no input. */
    xTaskCreatePinnedToCore(console_task, "jarvis_con", 4096, NULL, 2, NULL, 1);
    return ESP_OK;
}
