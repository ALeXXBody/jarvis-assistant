#include "jarvis_turn.h"
#include "jarvis_audio.h"
#include "jarvis_state.h"
#include "jarvis_ws.h"
#include "jarvis_settings.h"

#include "esp_log.h"

static const char *TAG = "turn";

void jarvis_turn_init(void)
{
    /* The mic's silence gate ends the utterance for us. */
    jarvis_audio_set_silence_cb(jarvis_turn_stop_listening);
}

void jarvis_turn_start_listening(void)
{
    jarvis_state_t st = jarvis_state_get();
    if (st == JVS_LISTENING) {
        return;
    }
    if (!jarvis_ws_connected()) {
        jarvis_state_status("No link to J.A.R.V.I.S.");
        return;
    }

    /* Barge-in: cancel anything still speaking/thinking. */
    if (st == JVS_SPEAKING || st == JVS_THINKING) {
        jarvis_ws_send_stop_run();
        jarvis_audio_flush();
    }

    const jarvis_settings_t *cfg = jarvis_settings();
    jarvis_state_set(JVS_LISTENING);
    jarvis_state_status("Listening...");
    jarvis_ws_send_start(cfg ? cfg->conversation : NULL);
    jarvis_audio_start_capture();
    ESP_LOGI(TAG, "listening");
}

void jarvis_turn_stop_listening(void)
{
    if (jarvis_state_get() != JVS_LISTENING) {
        return;
    }
    jarvis_audio_stop_capture();
    jarvis_ws_send_stop();
    jarvis_state_set(JVS_THINKING);
    jarvis_state_status("Thinking...");
    ESP_LOGI(TAG, "listening -> thinking");
}

void jarvis_turn_abort(void)
{
    jarvis_audio_stop_capture();
    jarvis_audio_flush();
    if (jarvis_ws_connected()) {
        jarvis_ws_send_stop_run();
    }
    jarvis_state_set(JVS_IDLE);
    jarvis_state_status("Ready");
    ESP_LOGI(TAG, "aborted");
}

void jarvis_turn_toggle(void)
{
    switch (jarvis_state_get()) {
    case JVS_LISTENING:
        jarvis_turn_stop_listening();
        break;
    case JVS_THINKING:
    case JVS_SPEAKING:
        jarvis_turn_abort();
        break;
    default:
        jarvis_turn_start_listening();
        break;
    }
}

void jarvis_turn_approval(bool allow)
{
    jarvis_ws_send_approval(NULL, NULL, allow);
    jarvis_state_approval(false, allow ? "Approved" : "Denied");
    jarvis_state_push_line(allow ? "[+] approved" : "[-] denied");
}

void jarvis_turn_reset(void)
{
    jarvis_audio_stop_capture();
    jarvis_audio_flush();
    jarvis_state_set(JVS_IDLE);
}
