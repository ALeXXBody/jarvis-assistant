#include "jarvis_wake.h"
#include "jarvis_config.h"
#include "jarvis_turn.h"
#include "jarvis_settings.h"
#include "jarvis_ws.h"
#include "bsp_guition.h"

#include <stdio.h>
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
            ESP_LOGI(TAG, "tap at %u,%u", pt.x, pt.y);
            jarvis_turn_toggle();
        }
        was = now;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

/* --- serial console ------------------------------------------------------ */

static void console_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "console: l=listen s=stop x=abort a=allow d=deny +=vol -=vol");
    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        switch (ch) {
        case 'l': jarvis_turn_start_listening(); break;
        case 's': jarvis_turn_stop_listening();  break;
        case 'x': jarvis_turn_abort();           break;
        case 'a': jarvis_turn_approval(true);     break;
        case 'd': jarvis_turn_approval(false);    break;
        case 'r': jarvis_ws_start(jarvis_settings()->server_url); break;
        case '+': {
            const jarvis_settings_t *c = jarvis_settings();
            jarvis_settings_set_volume(c->volume + 5);
            bsp_audio_set_volume(jarvis_settings()->volume);
            break;
        }
        case '-': {
            const jarvis_settings_t *c = jarvis_settings();
            jarvis_settings_set_volume(c->volume - 5);
            bsp_audio_set_volume(jarvis_settings()->volume);
            break;
        }
        default:
            break;
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
