#include "jarvis_audio.h"
#include "jarvis_afe.h"
#include "jarvis_config.h"
#include "jarvis_settings.h"
#include "jarvis_state.h"
#include "jarvis_ws.h"
#include "bsp_guition.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "audio";

static TaskHandle_t s_capture_task;
static TaskHandle_t s_play_task;
static StreamBufferHandle_t s_play_buf;
static volatile bool s_capturing;
static volatile bool s_turn_done;
static jarvis_audio_silence_cb_t s_silence_cb;
static jarvis_audio_barge_cb_t s_barge_cb;
static volatile float s_mic_level;      /* 0..1, EMA of mic RMS (UI poll) */

/* ---------------- playback ------------------------------------------------ */

static void play_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[JARVIS_FRAME_BYTES * 4];
    uint8_t *buf = heap_caps_malloc(JARVIS_FRAME_BYTES * 4, MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = chunk;
    }

    while (true) {
        size_t got = xStreamBufferReceive(s_play_buf, buf, JARVIS_FRAME_BYTES * 4,
                                          pdMS_TO_TICKS(200));
        if (got > 0) {
            /* Hand the very same audio to the echo canceller as its far-end
             * reference before it becomes audible. With the AFE this lets the
             * mic stay open while we talk; without it this is a cheap no-op. */
            jarvis_afe_push_reference((const int16_t *)buf, got / sizeof(int16_t));
            bsp_audio_play(buf, got, 1000);
        } else if (s_turn_done) {
            /* Buffer drained and the server said the turn is finished. */
            s_turn_done = false;
            if (jarvis_state_get() == JVS_SPEAKING) {
                jarvis_state_set(JVS_IDLE);
            }
        }
    }
}

void jarvis_audio_play_pcm(const uint8_t *pcm, size_t len)
{
    if (!s_play_buf || !pcm || len == 0) {
        return;
    }
    /* Never block the WS task: drop if the ring is full (we're behind). */
    size_t sent = xStreamBufferSend(s_play_buf, pcm, len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "playback ring full, dropped %u bytes", (unsigned)(len - sent));
    }
}

void jarvis_audio_mark_done(void)
{
    s_turn_done = true;
}

void jarvis_audio_flush(void)
{
    if (s_play_buf) {
        xStreamBufferReset(s_play_buf);
    }
    /* The AEC reference ring is deliberately NOT flushed here. The play task
     * pushes the reference immediately before handing the same bytes to the
     * codec, so the ring only ever holds audio that is already at the DAC (and
     * still audible for a few frames) — flushing it would throw away the
     * reference for exactly the sound we most need to cancel. Audio that was
     * merely queued never reaches the ring in the first place. */
    s_turn_done = false;
}

/* ---------------- public mic level ---------------------------------------- */

/* Exponential-average of the mic input level, 0..1 (1 ≈ loud speech).
 * Non-blocking; the UI polls it for the waveform ring. */
float jarvis_audio_mic_level(void)
{
    if (jarvis_afe_running()) {
        return jarvis_afe_mic_level();
    }
    return s_mic_level;
}

/* ---------------- capture ------------------------------------------------- */

static inline int16_t rms_of(const int16_t *s, size_t n)
{
    if (n == 0) {
        return 0;
    }
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (int32_t)s[i] * s[i];
    }
    float mean = (float)acc / (float)n;
    return (int16_t)sqrtf(mean);
}

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t frame[JARVIS_FRAME_SAMPLES];

    uint32_t silence_ms = 0;
    uint32_t elapsed_ms = 0;
    uint32_t next_level_log_ms = 1000;
    bool speech_seen = false;

    while (true) {
        if (!s_capturing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int bytes = bsp_audio_record((uint8_t *)frame, sizeof(frame), 100);
        if (bytes <= 0) {
            continue;
        }
        size_t samples = bytes / 2;
        uint32_t frame_ms = (uint32_t)((samples * 1000) / JARVIS_SAMPLE_RATE);
        elapsed_ms += frame_ms;

        /* Stream to the bridge. */
        jarvis_ws_send_audio((const uint8_t *)frame, (size_t)bytes);

        int rms = rms_of(frame, samples);
        if (elapsed_ms >= next_level_log_ms) {
            ESP_LOGI(TAG, "mic level rms=%d (speech>=%d, silence<%d) at %u ms",
                     rms, JARVIS_RMS_SPEECH, JARVIS_RMS_SILENCE,
                     (unsigned)elapsed_ms);
            next_level_log_ms += 1000;
        }
        /* UI feedback: normalised, exponential average (γ≈0.6 per frame). */
        {
            float lv = (float)rms / 3000.0f;      /* ~0.3 → full scale      */
            if (lv > 1.0f) {
                lv = 1.0f;
            }
            s_mic_level = s_mic_level * 0.4f + lv * 0.6f;
        }
        if (rms >= JARVIS_RMS_SPEECH) {
            speech_seen = true;
            silence_ms = 0;
        } else if (rms < JARVIS_RMS_SILENCE) {
            silence_ms += frame_ms;
        }

        bool timeout = elapsed_ms >= JARVIS_MAX_LISTEN_MS;
        bool end_of_speech = speech_seen &&
                             silence_ms >= JARVIS_SILENCE_STOP_MS &&
                             elapsed_ms >= JARVIS_SILENCE_ARM_MS;

        if (timeout || end_of_speech) {
            ESP_LOGI(TAG, "capture stop (%s, %ums)",
                     timeout ? "timeout" : "silence", (unsigned)elapsed_ms);
            s_capturing = false;
            if (s_silence_cb) {
                s_silence_cb();
            }
            silence_ms = 0;
            elapsed_ms = 0;
            speech_seen = false;
        }
    }
}

#if JARVIS_HAVE_AFE
/* Raised from the AFE fetch task once the AFE has decided the utterance is
 * over, or that the user has started talking over a reply. */
static void afe_event_cb(jarvis_afe_event_t ev)
{
    if (ev == JARVIS_AFE_EV_END_OF_SPEECH) {
        s_capturing = false;
        if (s_silence_cb) {
            s_silence_cb();
        }
    } else if (ev == JARVIS_AFE_EV_BARGE_IN) {
        if (s_barge_cb) {
            s_barge_cb();
        }
    }
}
#endif

void jarvis_audio_start_capture(void)
{
    s_capturing = true;
    if (jarvis_afe_running()) {
        jarvis_afe_set_capturing(true);
    }
}

void jarvis_audio_stop_capture(void)
{
    s_capturing = false;
    if (jarvis_afe_running()) {
        jarvis_afe_set_capturing(false);
    }
}

bool jarvis_audio_capturing(void)
{
    return s_capturing;
}

void jarvis_audio_set_silence_cb(jarvis_audio_silence_cb_t cb)
{
    s_silence_cb = cb;
}

void jarvis_audio_set_barge_cb(jarvis_audio_barge_cb_t cb)
{
    s_barge_cb = cb;
}

void jarvis_audio_set_volume(int percent)
{
    bsp_audio_set_volume(percent);
}

/* ---------------- init ---------------------------------------------------- */

esp_err_t jarvis_audio_init(void)
{
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    const jarvis_settings_t *cfg = jarvis_settings();
    bsp_audio_set_volume(cfg ? cfg->volume : 75);

    s_play_buf = xStreamBufferCreate(JARVIS_TTS_RING_BYTES, 1);
    if (!s_play_buf) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(play_task, "jarvis_play", 4096, NULL, 6,
                                            &s_play_task, 0);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

#if JARVIS_HAVE_AFE
    /* Preferred path: the ESP-SR AFE owns the microphone (AEC + NS + VAD).
     * It reads I2S directly, so the legacy RMS capture task must NOT also be
     * created — two tasks draining the same I2S RX would each see half the
     * audio. */
    if (jarvis_afe_start() == ESP_OK) {
        jarvis_afe_set_event_cb(afe_event_cb);
        ESP_LOGI(TAG, "capture: ESP-SR AFE (AEC + NS + VAD), hands-free");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "AFE unavailable — falling back to the RMS silence gate");
#endif

    ok = xTaskCreatePinnedToCore(capture_task, "jarvis_cap", 4096, NULL, 7,
                                 &s_capture_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
