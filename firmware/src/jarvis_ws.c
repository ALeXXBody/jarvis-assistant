#include "jarvis_ws.h"
#include "jarvis_config.h"
#include "jarvis_state.h"
#include "jarvis_audio.h"
#include "jarvis_turn.h"
#include "jarvis_wifi.h"

#include <string.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws";

static esp_websocket_client_handle_t s_client;
static char   s_rx[JARVIS_WS_RX_BUF];
static size_t s_rx_len;
static char   s_run_id[64];
static char   s_approval_id[64];
static TickType_t     s_last_retry_log = 0;   /* tick since boot (rate-limit) */

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */
static void copy_str(cJSON *o, const char *key, char *dst, size_t cap)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(dst, v->valuestring, cap);
    }
}

static esp_err_t send_json(cJSON *root)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    int rc = esp_websocket_client_send_text(s_client, txt, (int)strlen(txt),
                                            pdMS_TO_TICKS(1000));
    cJSON_free(txt);
    return rc >= 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------------- */
/* inbound text events                                                       */
/* ------------------------------------------------------------------------- */
static void handle_agent_status(cJSON *o)
{
    const char *state = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(o, "state"));
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(o, "name"));
    if (!state) {
        return;
    }

    if (strcmp(state, "thinking") == 0) {
        jarvis_state_set(JVS_THINKING);
        jarvis_state_status("Thinking...");
    } else if (strcmp(state, "tool_use") == 0) {
        jarvis_state_set(JVS_THINKING);
        if (name && *name) {
            jarvis_state_tool(name);
            char buf[96];
            snprintf(buf, sizeof(buf), "Using %s...", name);
            jarvis_state_status(buf);
        }
    } else if (strcmp(state, "speaking") == 0) {
        jarvis_state_set(JVS_SPEAKING);
        jarvis_state_status("Speaking...");
    } else if (strcmp(state, "stopped") == 0) {
        jarvis_state_set(JVS_IDLE);
        jarvis_state_status("Ready");
    }
}

static void handle_done(cJSON *o)
{
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(o, "timing");
    if (cJSON_IsObject(timing)) {
        cJSON *secs = cJSON_GetObjectItemCaseSensitive(timing, "total_turn_seconds");
        if (cJSON_IsNumber(secs)) {
            jarvis_state_metrics((float)secs->valuedouble);
        }
        const char *reply = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(timing, "response_text"));
        if (reply && *reply) {
            jarvis_state_reply(reply);
            char line[JVS_LINE_MAX];
            snprintf(line, sizeof(line), "JARVIS: %.80s", reply);
            jarvis_state_push_line(line);
        }
    }

    /* TTS audio normally arrives before `done`; let playback drain to idle. */
    jarvis_audio_mark_done();
    if (jarvis_state_get() != JVS_LISTENING) {
        jarvis_state_set(JVS_IDLE);
    }
    jarvis_state_status("Ready");
}

static void handle_text(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) {
        ESP_LOGW(TAG, "bad JSON: %.80s", json);
        return;
    }

    const char *type = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(o, "type"));
    if (!type) {
        cJSON_Delete(o);
        return;
    }

    if (strcmp(type, "status") == 0) {
        const char *msg = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(o, "message"));
        if (msg && *msg) {
            jarvis_state_status(msg);
        }
    } else if (strcmp(type, "partial_transcript") == 0) {
        jarvis_state_partial(cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(o, "text")));
    } else if (strcmp(type, "transcript") == 0) {
        const char *text = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(o, "text"));
        jarvis_state_transcript(text);
        if (text && *text) {
            char line[JVS_LINE_MAX];
            snprintf(line, sizeof(line), "> %.88s", text);
            jarvis_state_push_line(line);
        }
    } else if (strcmp(type, "run_started") == 0) {
        copy_str(o, "run_id", s_run_id, sizeof(s_run_id));
        jarvis_state_status("Working...");
    } else if (strcmp(type, "agent_status") == 0) {
        handle_agent_status(o);
    } else if (strcmp(type, "approval_request") == 0) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(o, "data");
        if (cJSON_IsObject(data)) {
            copy_str(data, "id", s_approval_id, sizeof(s_approval_id));
            copy_str(data, "approval_id", s_approval_id, sizeof(s_approval_id));
            copy_str(data, "run_id", s_run_id, sizeof(s_run_id));
        }
        const char *desc = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(o, "message"));
        jarvis_state_approval(true, desc ? desc : "Approval needed");
        jarvis_state_push_line("! approval needed");
    } else if (strcmp(type, "error") == 0) {
        const char *msg = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(o, "message"));
        ESP_LOGW(TAG, "server error: %s", msg ? msg : "(none)");
        jarvis_state_error(msg ? msg : "server error");
    } else if (strcmp(type, "done") == 0) {
        handle_done(o);
    } else {
        ESP_LOGD(TAG, "unhandled event type '%s'", type);
    }

    cJSON_Delete(o);
}

/* ------------------------------------------------------------------------- */
/* websocket events                                                          */
/* ------------------------------------------------------------------------- */
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    (void)arg; (void)base;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        jarvis_state_conn(jarvis_wifi_connected(), true);
        jarvis_state_status("Linked to J.A.R.V.I.S.");
        s_rx_len = 0;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        jarvis_state_conn(jarvis_wifi_connected(), false);
        jarvis_state_status("Link lost - reconnecting...");
        jarvis_turn_reset();
        s_rx_len = 0;
        /* One WARNING per minute instead of per retry: the esp-websocket
         * client retries every ~3 s and the identical lines flood the log. */
        if ((xTaskGetTickCount() - s_last_retry_log) > pdMS_TO_TICKS(60000)) {
            s_last_retry_log = xTaskGetTickCount();
            ESP_LOGW(TAG, "disconnected (reconnecting)");
        }
        break;

    case WEBSOCKET_EVENT_DATA: {
        /* Ping/pong/close control frames carry no application payload. */
        if (ev->op_code == 0x09 || ev->op_code == 0x0a || ev->op_code == 0x08) {
            break;
        }
        if (ev->op_code == 0x02) {                 /* binary = TTS PCM */
            if (ev->data_len > 0) {
                jarvis_audio_play_pcm((const uint8_t *)ev->data_ptr, ev->data_len);
            }
            break;
        }
        if (ev->op_code == 0x01 || ev->op_code == 0x00) {   /* text (reassemble) */
            if (ev->payload_offset == 0) {
                s_rx_len = 0;
            }
            if (s_rx_len + (size_t)ev->data_len < sizeof(s_rx)) {
                memcpy(s_rx + s_rx_len, ev->data_ptr, ev->data_len);
                s_rx_len += ev->data_len;
            } else {
                ESP_LOGW(TAG, "text frame too large, dropped");
                s_rx_len = 0;
                break;
            }
            if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                s_rx[s_rx_len] = '\0';
                handle_text(s_rx);
                s_rx_len = 0;
            }
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        if ((xTaskGetTickCount() - s_last_retry_log) > pdMS_TO_TICKS(60000)) {
            s_last_retry_log = xTaskGetTickCount();
            ESP_LOGE(TAG, "transport error (type %d)",
                     ev ? ev->error_handle.error_type : -1);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */
esp_err_t jarvis_ws_start(const char *url)
{
    if (!url || !*url) {
        return ESP_ERR_INVALID_ARG;
    }
    /* a running client keeps its old URI: replace it so a new brain address
     * (saved on the setup page) takes effect                               */
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = JARVIS_WS_RECONNECT_MS,
        .network_timeout_ms = JARVIS_WS_NETWORK_TIMEOUT_MS,
        .disable_auto_reconnect = false,
        .buffer_size = JARVIS_WS_RX_BUF,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,   /* generous: matches the server's keepalive */
    };

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);

    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "client started for %s", url);
    return ESP_OK;
}

void jarvis_ws_stop(void)
{
    if (!s_client) {
        return;
    }
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
}

bool jarvis_ws_connected(void)
{
    return s_client && esp_websocket_client_is_connected(s_client);
}

/* ------------------------------------------------------------------------- */
/* outbound                                                                  */
/* ------------------------------------------------------------------------- */
esp_err_t jarvis_ws_send_start(const char *conversation)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(r, "type", "start");
    cJSON_AddNumberToObject(r, "sample_rate", JARVIS_SAMPLE_RATE);
    cJSON_AddStringToObject(r, "format", "pcm_s16le");
    cJSON_AddNumberToObject(r, "channels", JARVIS_CHANNELS);
    cJSON_AddStringToObject(r, "conversation",
                            (conversation && *conversation) ? conversation
                                                            : JARVIS_DEFAULT_CONVERSATION);
    return send_json(r);
}

esp_err_t jarvis_ws_send_audio(const void *data, size_t len)
{
    if (!jarvis_ws_connected() || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len,
                                           pdMS_TO_TICKS(500));
    return rc >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t jarvis_ws_send_stop(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(r, "type", "stop");
    return send_json(r);
}

esp_err_t jarvis_ws_send_stop_run(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(r, "type", "stop_run");
    return send_json(r);
}

esp_err_t jarvis_ws_send_approval(const char *run_id, const char *approval_id, bool allow)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(r, "type", "approval_decision");
    cJSON_AddStringToObject(r, "run_id",
                            (run_id && *run_id) ? run_id : s_run_id);
    cJSON_AddStringToObject(r, "approval_id",
                            (approval_id && *approval_id) ? approval_id : s_approval_id);
    cJSON_AddStringToObject(r, "decision", allow ? "allow" : "deny");
    return send_json(r);
}
