#include "jarvis_ota.h"
#include "jarvis_config.h"
#include "jarvis_settings.h"
#include "jarvis_state.h"
#include "jarvis_wifi.h"
#include "jarvis_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

/* Guards s_busy: only one check/flash at a time (console + auto-check). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static char s_running_ver[32];

/* The OTA needs to know the version of the image that is RUNNING, not a
 * number a previous build might have stamped - so the version is read from
 * the image's own app descriptor. release.sh stamps it deterministically
 * (firmware/version.txt -> PROJECT_VER -> descriptor = the release number).
 * Dev builds fall back to git describe ("v0.1.7-5-g..."), which is still
 * truthful; only when the descriptor is empty do we fall back to the
 * compile-time JARVIS_FW_VERSION macro. Nothing is remembered across
 * updates - what was on the panel before never matters.                  */
const char *jarvis_ota_running_version(void)
{
    if (!s_running_ver[0]) {
        const esp_app_desc_t *desc = esp_app_get_description();
        const char *real = (desc && desc->version[0]) ? desc->version : "";
        strlcpy(s_running_ver, real[0] ? real : JARVIS_FW_VERSION,
                sizeof(s_running_ver));
    }
    return s_running_ver;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Joins a base URL (with or without a trailing '/') and an asset name. */
static void url_join(char *dst, size_t cap, const char *base, const char *name)
{
    size_t n = base ? strlen(base) : 0;
    bool slash = n > 0 && base[n - 1] == '/';
    snprintf(dst, cap, "%s%s%s", base ? base : "", slash ? "" : "/", name);
}/* Returns <0 / 0 / >0 for a vs b, comparing up to four numeric components.
 * Non-numeric strings (e.g. "dev") compare as 0.0.0.0 — i.e. never "newer". */
static int version_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;

    const char *pa = a, *pb = b;
    for (int i = 0; i < 4; i++) {
        long va = strtol(pa, (char **)&pa, 10);
        long vb = strtol(pb, (char **)&pb, 10);
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
    }
    return 0;
}

static bool net_up(void)
{
    return jarvis_wifi_connected() || jarvis_ws_connected();
}

/* ------------------------------------------------------------------------- */
/* the check + flash task                                                    */
/* ------------------------------------------------------------------------- */
static void ota_task(void *arg)
{
    (void)arg;
    const char *base = jarvis_settings()->ota_url;

    if (!base || !*base) {
        ESP_LOGI(TAG, "no OTA URL configured; skipping");
        goto done;
    }

    char img_url[240];
    url_join(img_url, sizeof(img_url), base, "firmware.bin");

    const char *running = jarvis_ota_running_version();

    /* esp_https_ota streams the image; its FIRST bytes carry the app
     * descriptor — the same self-stamped version the running image reports.
     * No auxiliary version file, no server-side state: the incoming image
     * itself says what it is, so check and payload are one single GET.     */
    ESP_LOGI(TAG, "checking %s (running %s)", img_url, running);
    esp_http_client_config_t http = {
        .url = img_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = JARVIS_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .user_agent = JARVIS_OTA_USER_AGENT,
        .max_redirection_count = 10,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_app_desc_t inc;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "update check failed: %s", esp_err_to_name(err));
        goto done;
    }

    /* Nothing has been written to flash yet: get_img_desc inspects the
     * stream only, so a same-version image costs a few KB of traffic.     */
    err = esp_https_ota_get_img_desc(h, &inc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "incoming image has no valid app descriptor");
        esp_https_ota_abort(h);
        goto done;
    }
    if (version_cmp(inc.version, running) <= 0) {
        ESP_LOGI(TAG, "image %s is not newer than running %s - up to date",
                 inc.version, running);
        esp_https_ota_abort(h);
        goto done;
    }

    ESP_LOGW(TAG, "updating %s -> %s from %s", running, inc.version, img_url);
    jarvis_state_status("Updating firmware...");
    err = esp_https_ota_perform(h);
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "new image failed validation");
            jarvis_state_error("Firmware update failed validation");
            goto done;
        }
        ESP_LOGI(TAG, "update applied; rebooting into %s", inc.version);
        jarvis_state_status("Update installed - rebooting");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        jarvis_state_error("Firmware update failed");
    }

done:
    taskENTER_CRITICAL(&s_mux);
    s_busy = false;
    taskEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */
esp_err_t jarvis_ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool pending = false;

    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK) {
        pending = (st == ESP_OTA_IMG_PENDING_VERIFY);
    }

    ESP_LOGI(TAG, "running fw=%s part=%s @0x%08lx %s",
             jarvis_ota_running_version(),
             run ? run->label : "?",
             (unsigned long)(run ? run->address : 0),
             pending ? "(PENDING VERIFY)" : "");
    return ESP_OK;
}

void jarvis_ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;

    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* normal boot, or rollback disabled */
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new image confirmed healthy: %s", esp_err_to_name(err));
}

bool jarvis_ota_check_async(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_busy) {
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "an update check is already running");
        return false;
    }
    s_busy = true;
    taskEXIT_CRITICAL(&s_mux);

    if (xTaskCreate(ota_task, "jarvis_ota", 10240, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start OTA task");
        taskENTER_CRITICAL(&s_mux);
        s_busy = false;
        taskEXIT_CRITICAL(&s_mux);
        return false;
    }
    return true;
}

static void autocheck_task(void *arg)
{
    (void)arg;
    /* Check whenever the panel is online, then every 30 minutes. Wi-Fi comes
     * up after boot (or later, via the setup page), so a single check at boot
     * would never run on a panel that joins the network afterwards.          */
    while (true) {
        if (net_up()) {
            jarvis_ota_check_async();
            vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(15 * 1000));
        }
    }
}

void jarvis_ota_autocheck_start(void)
{
    const char *base = jarvis_settings()->ota_url;
    if (!base || !*base) {
        ESP_LOGI(TAG, "OTA disabled (no URL); set JARVIS_DEFAULT_OTA_URL");
        return;
    }
    if (xTaskCreate(autocheck_task, "jarvis_ota_chk", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start auto-check task");
    }
}
