/*
 * Audio I/O for the JARVIS satellite.
 *
 *  capture : ES8311 mic -> 16 kHz mono S16LE -> WebSocket binary frames.
 *            Runs only while state == JVS_LISTENING. Simple RMS gate
 *            auto-stops the utterance after JARVIS_SILENCE_STOP_MS of silence
 *            and fires the registered silence callback.
 *  playback: WebSocket binary TTS PCM -> ring buffer -> ES8311 DAC.
 *
 * Both ends run as their own FreeRTOS task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t jarvis_audio_init(void);

/* Begin/stop streaming the microphone to the server. */
void jarvis_audio_start_capture(void);
void jarvis_audio_stop_capture(void);
bool jarvis_audio_capturing(void);

/* Enqueue a chunk of TTS PCM for playback (called from the WS task). */
void jarvis_audio_play_pcm(const uint8_t *pcm, size_t len);

/* Marks the current turn's audio as complete; playback task drops to idle
 * once the buffer drains (called from the WS task on the "done" event). */
void jarvis_audio_mark_done(void);

/* Flush any queued playback (barge-in / stop). */
void jarvis_audio_flush(void);

/* Called when the silence gate decides the utterance ended. */
typedef void (*jarvis_audio_silence_cb_t)(void);
void jarvis_audio_set_silence_cb(jarvis_audio_silence_cb_t cb);

/* Called when the user starts speaking while NOT capturing — i.e. they are
 * talking over a reply. Only fires on the ESP-SR AFE build (jarvis-panel-sr),
 * where the echo canceller keeps the mic usable during playback; on the
 * push-to-talk build the mic is closed while J.A.R.V.I.S. speaks, so this
 * never fires and the handler is optional. */
typedef void (*jarvis_audio_barge_cb_t)(void);
void jarvis_audio_set_barge_cb(jarvis_audio_barge_cb_t cb);

/* 0..100, non-blocking. */
void jarvis_audio_set_volume(int percent);

/* Mic input level as an exponential average, 0..1 (1 ≈ loud speech).
 * UI polls this for the waveform ring; non-blocking. */
float jarvis_audio_mic_level(void);