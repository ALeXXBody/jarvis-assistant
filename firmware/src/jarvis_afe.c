/*
 * ESP-SR audio front-end (AFE) wrapper — see jarvis_afe.h for the contract.
 *
 * This file compiles to inert stubs when CONFIG_JARVIS_ENABLE_ESP_SR is off,
 * so the default `jarvis-panel` firmware carries none of the esp-sr code or
 * models. Build `jarvis-panel-sr` to enable it.
 *
 * Pipeline
 * --------
 *   feed task  : ES8311 mic (16 kHz mono) + playback reference, interleaved
 *                as [mic, ref] pairs, pushed into afe->feed(). Runs forever.
 *   fetch task : afe->fetch() returns the echo-cancelled / denoised single
 *                channel stream plus the VAD verdict. While capturing we
 *                stream it to the bridge; the VAD decides end-of-speech. While
 *                not capturing a speech onset after a quiet period is reported
 *                as a barge-in.
 *
 * The AFE has to be fed continuously for the AEC filter to stay converged, so
 * both tasks always run once started — `jarvis_afe_set_capturing()` only
 * decides whether audio is forwarded to the bridge.
 */
#include "jarvis_afe.h"
#include "jarvis_config.h"

#if JARVIS_HAVE_AFE

#include "jarvis_ws.h"
#include "bsp_guition.h"

#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_vad.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "afe";

/* The AFE input is described by a channel-format string: one letter per
 * channel, 'M' = microphone, 'R' = playback reference. "MR" = one mic then
 * one reference, which matches the ES8311 full-duplex I2S setup (mic in, DAC
 * out, same 16 kHz clock domain). */
#define JARVIS_AFE_INPUT_FORMAT "MR"

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t        *s_afe_data;
static srmodel_list_t           *s_models;

static int s_feed_chunk;   /* samples per channel per feed() call */
static int s_feed_nch;     /* channels we hand to feed() (mic + ref) */

static StreamBufferHandle_t s_ref;      /* playback reference ring */
static StaticStreamBuffer_t s_ref_struct;
static uint8_t              s_ref_storage[JARVIS_AFE_REF_BYTES + 1];

static volatile bool s_capturing;
static volatile float s_level;          /* 0..1, EMA of mic RMS for the UI */

static jarvis_afe_event_cb_t s_event_cb;

/* Playback timing, for the barge-in settle guard (tick counts, 0 = never). */
static volatile uint32_t s_last_push_tick;
static volatile uint32_t s_play_start_tick;

static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;

static volatile bool s_running;

/* ---------------- public helpers ----------------------------------------- */

bool jarvis_afe_running(void)
{
    return s_running;
}

float jarvis_afe_mic_level(void)
{
    return s_level;
}

void jarvis_afe_set_event_cb(jarvis_afe_event_cb_t cb)
{
    s_event_cb = cb;
}

void jarvis_afe_set_capturing(bool on)
{
    if (!s_running) {
        return;
    }
    /* No AFE reset here on purpose. The fetch task only endpoints after it has
     * seen a *fresh* speech frame in this capture (speech_seen starts false),
     * so a stale VAD verdict cannot end an utterance that never began — and
     * resetting the AFE buffers would throw away the AEC state that the feed
     * task has been converging the whole time. */
    s_capturing = on;
}

void jarvis_afe_push_reference(const int16_t *pcm, size_t samples)
{
    if (!s_running || !pcm || samples == 0) {
        return;
    }

    /* Track playback bursts so the fetch task knows how long the echo
     * canceller has had to converge on the current run of speech. */
    uint32_t now = xTaskGetTickCount();
    if (s_last_push_tick == 0 ||
        (now - s_last_push_tick) > pdMS_TO_TICKS(JARVIS_AFE_PLAY_GAP_MS)) {
        s_play_start_tick = now;
    }
    s_last_push_tick = now;

    size_t bytes = samples * sizeof(int16_t);
    /* Bounded block: the play and feed tasks are paced by the same I2S clock
     * (16 kHz full duplex), so a short wait keeps the reference lined up with
     * what is actually audible instead of letting the ring drift. */
    size_t sent = xStreamBufferSend(s_ref, pcm, bytes, pdMS_TO_TICKS(20));
    if (sent < bytes) {
        ESP_LOGW(TAG, "reference ring overrun, lost %u bytes",
                 (unsigned)(bytes - sent));
    }
}

/* ---------------- feed task ---------------------------------------------- */

static void afe_feed_task(void *arg)
{
    (void)arg;

    const size_t mic_bytes   = (size_t)s_feed_chunk * sizeof(int16_t);
    const size_t ref_bytes   = mic_bytes;
    const size_t frame_bytes = mic_bytes * (size_t)s_feed_nch;

    int16_t *mic   = heap_caps_malloc(mic_bytes, MALLOC_CAP_INTERNAL);
    int16_t *ref   = heap_caps_malloc(ref_bytes, MALLOC_CAP_INTERNAL);
    int16_t *frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL);
    if (!mic || !ref || !frame) {
        ESP_LOGE(TAG, "feed buffers: out of memory");
        if (mic)   free(mic);
        if (ref)   free(ref);
        if (frame) free(frame);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed: %d samples/channel, %d channels", s_feed_chunk, s_feed_nch);

    while (true) {
        /* Zero the whole interleaved frame first: we only know how to fill
         * channels 0 (mic) and 1 (reference), and any extra channel the AFE
         * geometry might declare should be silence rather than stack garbage. */
        memset(frame, 0, frame_bytes);

        /* --- microphone (16 kHz mono) ---------------------------------- */
        int got = bsp_audio_record((uint8_t *)mic, mic_bytes, 100);
        if (got <= 0) {
            memset(mic, 0, mic_bytes);
        } else if ((size_t)got < mic_bytes) {
            memset((uint8_t *)mic + got, 0, mic_bytes - (size_t)got);
        }

        /* --- playback reference ---------------------------------------- */
        size_t rgot = xStreamBufferReceive(s_ref, ref, ref_bytes, 0);
        if (rgot == 0) {
            memset(ref, 0, ref_bytes);
        } else if (rgot < ref_bytes) {
            memset((uint8_t *)ref + rgot, 0, ref_bytes - rgot);
        }

        /* --- interleave [mic, ref] pairs ------------------------------- */
        if (s_feed_nch >= 2) {
            for (int i = 0; i < s_feed_chunk; i++) {
                frame[i * s_feed_nch + 0] = mic[i];
                frame[i * s_feed_nch + 1] = ref[i];
            }
        } else {
            memcpy(frame, mic, mic_bytes);
        }

        s_afe->feed(s_afe_data, frame);
    }
}

/* ---------------- fetch task --------------------------------------------- */

static void afe_fetch_task(void *arg)
{
    (void)arg;

    bool     speech_seen = false;   /* speech observed in this utterance */
    bool     prev_speech = false;
    uint32_t elapsed_ms  = 0;       /* while capturing */
    uint32_t quiet_ms    = 0;       /* continuous non-speech, always */

    while (true) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->data_size <= 0) {
            continue;
        }

        const int16_t *pcm = res->data;
        size_t samples     = (size_t)res->data_size / sizeof(int16_t);
        if (samples == 0) {
            continue;
        }
        uint32_t frame_ms = (uint32_t)((samples * 1000) / JARVIS_SAMPLE_RATE);
        bool speech = (res->vad_state == VAD_SPEECH);

        /* UI level: same normalisation as the legacy RMS gate so the on-screen
         * waveform ring looks the same either way. */
        {
            int64_t acc = 0;
            for (size_t i = 0; i < samples; i++) {
                acc += (int32_t)pcm[i] * pcm[i];
            }
            float rms = sqrtf((float)acc / (float)samples);
            float lv  = rms / 3000.0f;
            if (lv > 1.0f) {
                lv = 1.0f;
            }
            s_level = s_level * 0.4f + lv * 0.6f;
        }

        if (s_capturing) {
            elapsed_ms += frame_ms;

            jarvis_ws_send_audio(pcm, (size_t)res->data_size);

            if (speech) {
                speech_seen = true;
            }

            /* The AFE itself only reports VAD_SILENCE once it has seen
             * vad_min_noise_ms of quiet, so the first silence frame after
             * speech is already a complete endpoint. */
            bool end_of_speech = speech_seen && !speech;
            bool timeout       = elapsed_ms >= JARVIS_MAX_LISTEN_MS;

            if (end_of_speech || timeout) {
                ESP_LOGI(TAG, "capture stop (%s, %ums)",
                         timeout ? "timeout" : "vad",
                         (unsigned)elapsed_ms);
                s_capturing = false;
                speech_seen = false;
                elapsed_ms  = 0;
                if (s_event_cb) {
                    s_event_cb(JARVIS_AFE_EV_END_OF_SPEECH);
                }
            }
        } else {
            /* Barge-in: a speech onset while we are not deliberately listening.
             *
             * Three conditions, each ruling out one way we could interrupt
             * ourselves instead of the user:
             *   quiet_ms >= ARM  the user's own previous utterance has died down;
             *   rising edge      it is an onset, not one continuous speech
             *                    segment still being reported from before;
             *   settled          playback has been running long enough for the
             *                    AEC to model the speaker, so "speech" means a
             *                    person started talking rather than echo
             *                    leaking through at the start of a reply.
             */
            uint32_t now = xTaskGetTickCount();
            bool playing = (s_last_push_tick != 0) &&
                ((now - s_last_push_tick) <= pdMS_TO_TICKS(JARVIS_AFE_PLAY_GAP_MS));
            bool settled = !playing ||
                ((now - s_play_start_tick) >= pdMS_TO_TICKS(JARVIS_AFE_BARGE_SETTLE_MS));

            if (speech && !prev_speech && quiet_ms >= JARVIS_AFE_BARGE_ARM_MS &&
                settled) {
                ESP_LOGI(TAG, "barge-in detected");
                if (s_event_cb) {
                    s_event_cb(JARVIS_AFE_EV_BARGE_IN);
                }
            }
            speech_seen = false;
            elapsed_ms  = 0;
        }

        if (speech) {
            quiet_ms = 0;
        } else if (quiet_ms < UINT32_MAX - frame_ms) {
            quiet_ms += frame_ms;
        }
        prev_speech = speech;
    }
}

/* ---------------- lifecycle ---------------------------------------------- */

esp_err_t jarvis_afe_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "esp_srmodel_init(\"model\") failed — was the model "
                      "image flashed? (partitions.sr.csv + the model image)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "models loaded from `model` partition (%d)", s_models->num);

    afe_config_t *cfg = afe_config_init(JARVIS_AFE_INPUT_FORMAT, s_models,
                                        AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->aec_init            = true;
    cfg->ns_init             = true;
    cfg->vad_init            = true;
    cfg->agc_init            = true;
    cfg->vad_min_speech_ms   = JARVIS_AFE_VAD_SPEECH_MS;
    cfg->vad_min_noise_ms    = JARVIS_AFE_VAD_SILENCE_MS;
    cfg->vad_delay_ms        = JARVIS_AFE_VAD_DELAY_MS;
    cfg->fixed_output_channel = true;
    /* The AEC / esp-dl state is a few hundred kB — keep it out of internal RAM
     * so it does not fight the display and Wi-Fi buffers. */
    cfg->memory_alloc_mode   = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core  = 1;
    cfg->afe_perferred_priority = 5;

    s_afe = esp_afe_handle_from_config(cfg);
    if (!s_afe) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg);
        return ESP_FAIL;
    }

    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "create_from_config failed");
        return ESP_FAIL;
    }

    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_nch   = s_afe->get_feed_channel_num(s_afe_data);
    if (s_feed_chunk <= 0 || s_feed_nch <= 0) {
        ESP_LOGE(TAG, "bad AFE geometry: chunk=%d nch=%d", s_feed_chunk, s_feed_nch);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AFE up: %d Hz, feed chunk %d, %d channels",
             s_afe->get_samp_rate(s_afe_data), s_feed_chunk, s_feed_nch);

    s_ref = xStreamBufferCreateStatic(sizeof(s_ref_storage), 1,
                                      s_ref_storage, &s_ref_struct);
    if (!s_ref) {
        ESP_LOGE(TAG, "reference ring alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_running = true;

    if (xTaskCreatePinnedToCore(afe_feed_task, "jarvis_afe_feed", 4096, NULL, 7,
                                &s_feed_task, 1) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(afe_fetch_task, "jarvis_afe_fetch", 4608, NULL, 6,
                                &s_fetch_task, 1) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else /* !JARVIS_HAVE_AFE — inert stubs so callers need no #ifdefs ---------- */

bool jarvis_afe_running(void) { return false; }
float jarvis_afe_mic_level(void) { return 0.0f; }
void jarvis_afe_set_event_cb(jarvis_afe_event_cb_t cb) { (void)cb; }
void jarvis_afe_set_capturing(bool on) { (void)on; }
void jarvis_afe_push_reference(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples;
}

esp_err_t jarvis_afe_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* JARVIS_HAVE_AFE */
