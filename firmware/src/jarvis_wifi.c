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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

/* A wrong password used to spin the retry timer every 3 s forever: the
 * setup-sheet scan then raced the reconnect attempt on the C6 and came back
 * empty ("no networks found") and the list could not be recovered without
 * reflashing. The first JOIN_FAST_RETRIES failures re-arm fast so a good
 * incident (momentary AP drop) self-heals; after that attempts slow down to
 * one every 45 s, so the radio is mostly idle and scans work. A new
 * jarvis_wifi_start() call (user retyped the password) or a successful join
 * resets the counter.                                                    */
#define JOIN_FAST_RETRIES   5
#define JOIN_FAST_RETRY_MS  (3000 * 1000)
#define JOIN_SLOW_RETRY_MS  (45000 * 1000)

static bool s_connected;
static bool s_started;
static bool s_want_join;   /* credentials present -> join on STA_START     */
static int  s_join_fails;  /* consecutive failed join attempts             */

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
    case WIFI_REASON_ASSOC_FAIL:            return "AP refused (2.4 GHz?)";
    case WIFI_REASON_BEACON_TIMEOUT:        return "AP dropping (signal?)";
    default:                                return "reconnecting";
    }
}

static esp_netif_t       *s_netif;        /* created once              */
static bool               s_inited;       /* esp_wifi_init + mode done  */
static bool               s_handlers;     /* event handlers registered  */
static esp_timer_handle_t s_retry_timer;  /* reconnect back-off         */
static bool               s_slowed_logged;/* prevent repeat slow-mode W  */

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s_want_join) {
        return;                        /* scan silenced the join loop */
    }
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_join) {
            jarvis_state_status("wifi: connecting");
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "station up - no credentials, idle for scanning");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        int reason = ev ? ev->reason : 0;
        s_connected = false;
        jarvis_state_conn(false, false);
        if (!s_want_join) {
            return;                       /* scan kick or idle: stay quiet */
        }
        s_join_fails++;
        TickType_t delay_ms = JOIN_FAST_RETRY_MS;
        bool slowed = false;
        if (s_join_fails > JOIN_FAST_RETRIES) {
            delay_ms = JOIN_SLOW_RETRY_MS;
            slowed = true;
        }
        if (s_join_fails <= 3 || s_join_fails == JOIN_FAST_RETRIES ||
            slowed != s_slowed_logged) {
            ESP_LOGW(TAG, "disconnected (%d: %s)%s", reason,
                     reason_text(reason),
                     slowed ? " - retrying slowly so scans can run" : "");
            s_slowed_logged = slowed;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "wifi: %s", reason_text(reason));
        jarvis_state_error(buf);
        if (reason != WIFI_REASON_AUTH_LEAVE && s_retry_timer) {
            esp_timer_stop(s_retry_timer);            /* ignore "not running" */
            esp_timer_start_once(s_retry_timer, delay_ms);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_join_fails = 0;
        s_connected = true;
        jarvis_state_conn(true, false);
    }
}

/* One-time bring-up: netif, driver, handlers, mode. Safe to call again:
 * every step is skipped when already done, and a failure leaves no state
 * that makes the next call fail the same way.                              */
static esp_err_t wifi_bring_up_once(void)
{
    esp_err_t err = jarvis_net_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_netif) {
        ESP_LOGE(TAG, "station netif could not be created");
        return ESP_FAIL;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (on-board C6 slave present?)",
                 esp_err_to_name(err));
        jarvis_state_error("wifi: C6 link failed");
        return err;
    }

    if (!s_handlers) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  on_wifi_event, NULL, NULL);
        if (err == ESP_OK) {
            err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      on_wifi_event, NULL, NULL);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "event handler registration: %s", esp_err_to_name(err));
            return err;
        }
        s_handlers = true;
    }

    if (!s_retry_timer) {
        esp_timer_create_args_t targs = {
            .callback = retry_cb,
            .name = "wifi_retry",
        };
        if (esp_timer_create(&targs, &s_retry_timer) != ESP_OK) {
            s_retry_timer = NULL;         /* reconnect then relies on events */
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: mode failed");
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t jarvis_wifi_start(const char *ssid, const char *pass)
{
    esp_err_t err;

    if (!ssid) {
        ssid = "";
    }
    s_want_join = (ssid[0] != '\0');
    s_join_fails = 0;              /* fresh attempt (user retyped password) */

    if (!s_inited) {
        err = wifi_bring_up_once();
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: config failed");
        return err;
    }

    if (s_started) {
        if (s_want_join) {
            esp_wifi_disconnect();
            esp_wifi_connect();
            ESP_LOGI(TAG, "reconfigured, joining \"%s\"", ssid);
        }
        return ESP_OK;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        jarvis_state_error("wifi: start failed");
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "station started%s%s", s_want_join ? ", joining \"" : "",
             s_want_join ? ssid : "");
    return ESP_OK;
}

bool jarvis_wifi_connected(void) { return s_connected; }
bool jarvis_wifi_started(void)   { return s_started; }

esp_err_t jarvis_wifi_scan(jarvis_wifi_ap_t *out, size_t *cnt, size_t max)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out || !cnt) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A scan against a radio that is mid-handshake (wrong password, or AP
     * in the middle of dropping us) reliably returns zero APs. Silence the
     * join loop first so the setup sheet gets a usable list; when the scan
     * completes the join resumes after 2 s unless the credentials are in
     * slow mode (then a user action re-arms it from scratch).             */
    bool resume = s_want_join && !s_connected;
    if (resume) {
        s_want_join = false;
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        esp_wifi_disconnect();
    }

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&sc, true /* block */);
    if (resume) {
        s_want_join = true;
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, 2 * 1000 * 1000);  /* 2 s later */
        }
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t n = 0;
    err = esp_wifi_scan_get_ap_num(&n);
    if (err == ESP_OK && n == 0) {
        *cnt = 0;
        return ESP_OK;
    }
    if (n > 64) {
        n = 64;    /* bounded; our UI lists up to 8, keep memory sane */
    }

    /* static, not stack: 64 records are ~6 KB and the scan runs in a task
     * with a small stack (a stack overflow here rebooted the panel).      */
    static wifi_ap_record_t recs[64];
    esp_wifi_scan_get_ap_records(&n, recs);    /* frees the scan memory  */

    /* by RSSI (api returns sorted? not guaranteed on remote) — insertion sort */
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t t = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].rssi < t.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = t;
    }

    size_t k = 0;
    for (uint16_t i = 0; i < n && k < max; i++) {
        memset(out[k].ssid, 0, sizeof(out[k].ssid));
        strlcpy(out[k].ssid, (char *)(recs[i].ssid[0] ? (const void *)recs[i].ssid
                                                      : "(hidden)"),
                sizeof(out[k].ssid));
        out[k].rssi = recs[i].rssi;
        k++;
    }
    *cnt = k;
    return ESP_OK;
}

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

esp_err_t jarvis_wifi_scan(jarvis_wifi_ap_t *out, size_t *cnt, size_t max)
{
    (void)out; (void)cnt; (void)max;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* JARVIS_HAVE_WIFI */
