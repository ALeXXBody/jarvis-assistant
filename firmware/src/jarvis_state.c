#include "jarvis_state.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "state";

static SemaphoreHandle_t s_lock;
static jarvis_snapshot_t s_snap;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void jarvis_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = JVS_BOOTING;
    s_snap.battery_pct = -1;
    strlcpy(s_snap.status, "Booting", sizeof(s_snap.status));
}

const char *jarvis_state_name(jarvis_state_t s)
{
    switch (s) {
    case JVS_BOOTING:   return "booting";
    case JVS_IDLE:      return "idle";
    case JVS_LISTENING: return "listening";
    case JVS_THINKING:  return "thinking";
    case JVS_SPEAKING:  return "speaking";
    case JVS_ERROR:     return "error";
    default:            return "?";
    }
}

void jarvis_state_set(jarvis_state_t s)
{
    lock();
    if (s_snap.state != s) {
        ESP_LOGI(TAG, "state %s -> %s", jarvis_state_name(s_snap.state), jarvis_state_name(s));
        s_snap.state = s;
    }
    unlock();
}

jarvis_state_t jarvis_state_get(void)
{
    lock();
    jarvis_state_t s = s_snap.state;
    unlock();
    return s;
}

void jarvis_state_copy(jarvis_snapshot_t *out)
{
    if (!out) {
        return;
    }
    lock();
    *out = s_snap;
    unlock();
}

void jarvis_state_status(const char *msg)
{
    if (!msg) return;
    lock();
    strlcpy(s_snap.status, msg, sizeof(s_snap.status));
    unlock();
}

void jarvis_state_tool(const char *name)
{
    if (!name) return;
    lock();
    strlcpy(s_snap.tool, name, sizeof(s_snap.tool));
    unlock();
}

void jarvis_state_partial(const char *text)
{
    if (!text) return;
    lock();
    strlcpy(s_snap.user_text, text, sizeof(s_snap.user_text));
    unlock();
}

void jarvis_state_transcript(const char *text)
{
    if (!text) return;
    lock();
    strlcpy(s_snap.user_text, text, sizeof(s_snap.user_text));
    unlock();
    /* The WS layer decides how (and whether) this becomes a display line. */
}

void jarvis_state_reply(const char *text)
{
    if (!text) return;
    lock();
    strlcpy(s_snap.reply_text, text, sizeof(s_snap.reply_text));
    unlock();
}

void jarvis_state_error(const char *msg)
{
    if (!msg) return;
    lock();
    strlcpy(s_snap.error, msg, sizeof(s_snap.error));
    strlcpy(s_snap.status, msg, sizeof(s_snap.status));
    s_snap.state = JVS_ERROR;
    unlock();
}

void jarvis_state_conn(bool wifi, bool ws)
{
    lock();
    s_snap.wifi_connected = wifi;
    s_snap.ws_connected = ws;
    unlock();
}

void jarvis_state_approval(bool pending, const char *desc)
{
    lock();
    s_snap.approval_pending = pending;
    if (desc) {
        strlcpy(s_snap.status, desc, sizeof(s_snap.status));
    }
    unlock();
}

void jarvis_state_metrics(float total_seconds)
{
    lock();
    s_snap.last_total_seconds = total_seconds;
    unlock();
}

void jarvis_state_set_battery(int pct)
{
    lock();
    s_snap.battery_pct = pct;
    unlock();
}

void jarvis_state_push_line(const char *line)
{
    if (!line) return;
    lock();
    for (int i = 1; i < JVS_LINES; i++) {
        memcpy(s_snap.lines[i - 1], s_snap.lines[i], JVS_LINE_MAX);
    }
    strlcpy(s_snap.lines[JVS_LINES - 1], line, JVS_LINE_MAX);
    unlock();
}
