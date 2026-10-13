#include "jarvis_ota.h"
#include "jarvis_config.h"
#include "jarvis_settings.h"
#include "jarvis_state.h"
#include "jarvis_wifi.h"
#include "jarvis_ws.h"

#include <ctype.h>
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

const char *jarvis_ota_running_version(void)
{
    return JARVIS_FW_VERSION;
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
}

/* Returns <0 / 0 / >0 for a vs b, comparing up to four numeric components.
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

/* GETs a small text body (used for version.txt). Returns true on HTTP 200. */
static bool http_get_text_depth(const char *url, char *out, size_t cap, int depth);

static bool http_get_text(const char *url, char *out, size_t cap)
{
    return http_get_text_depth(url, out, cap, 0);
}

static bool http_get_text_depth(const char *url, char *out, size_t cap, int depth)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = JARVIS_OTA_HTTP_TIMEOUT_MS,
        .user_agent = JARVIS_OTA_USER_AGENT,
        .keep_alive_enable = true,
        .max_redirection_count = 10,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        /* fetch_headers() returns -1 for a chunked body, which is fine: the
         * read loop below handles both fixed and chunked responses. */
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && depth < 4) {
            /* GitHub's releases/latest/download sends a redirect to the asset
             * host. esp_http_client_open() does not follow redirects itself,
             * so follow the Location header here.                          */
            char loc[256] = { 0 };
            char *locp = NULL;
            if (esp_http_client_get_header(c, "Location", &locp) == ESP_OK && locp) {
                strlcpy(loc, locp, sizeof(loc));
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return http_get_text_depth(loc, out, cap, depth + 1);
            }
        }
        if (status == 200) {
            int total = 0;
            while (total < (int)cap - 1) {
                int r = esp_http_client_read(c, out + total, (int)cap - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            out[total] = '\0';
            while (total > 0 && isspace((unsigned char)out[total - 1])) {
                out[--total] = '\0';
            }
            ok = total > 0;
        } else {
            ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        }
    } else {
        ESP_LOGW(TAG, "GET %s: connection failed", url);
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
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

    char ver_url[240];
    char img_url[240];
    url_join(ver_url, sizeof(ver_url), base, "version.txt");
    url_join(img_url, sizeof(img_url), base, "firmware.bin");

    const char *running = jarvis_ota_running_version();
    char remote[32] = { 0 };

    if (!http_get_text(ver_url, remote, sizeof(remote))) {
        ESP_LOGW(TAG, "could not read %s (offline, private repo, or no asset)",
                 ver_url);
        goto done;
    }

    ESP_LOGI(TAG, "running %s, latest %s", running, remote);
    if (version_cmp(remote, running) <= 0) {
        ESP_LOGI(TAG, "firmware up to date");
        goto done;
    }

    ESP_LOGW(TAG, "updating %s -> %s from %s", running, remote, img_url);
    jarvis_state_status("Updating firmware...");

    esp_http_client_config_t http = {
        .url = img_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = JARVIS_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .user_agent = JARVIS_OTA_USER_AGENT,
        .max_redirection_count = 10,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update applied; rebooting into %s", remote);
        jarvis_state_status("Update installed - rebooting");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
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

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "running fw=%s app_ver=%s part=%s @0x%08lx %s",
             jarvis_ota_running_version(),
             desc ? desc->version : "?",
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
