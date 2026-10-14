/*
 * ESP-SR audio front-end (AFE) wrapper — optional.
 *
 * The AFE is Espressif's on-device audio pipeline: acoustic echo cancellation
 * (AEC), noise suppression (NS) and a neural voice-activity detector (VAD).
 * Wrapping it here lets the rest of the firmware stay agnostic: every entry
 * point in this header is an inert stub when the firmware was built without
 * CONFIG_JARVIS_ENABLE_ESP_SR, so callers need no #ifdefs.
 *
 * Why the AFE exists in a voice satellite:
 *
 *   * AEC subtracts the panel's own playback from the microphone signal. That
 *     is what lets the mic stay open while J.A.R.V.I.S. is talking, which is
 *     the whole point — you can simply start speaking to interrupt instead of
 *     tapping the screen.
 *   * NS cleans the signal before it is streamed to the bridge for ASR.
 *   * VAD gives a real end-of-speech signal, steadier than an RMS threshold,
 *     which tightens turn latency.
 *
 * The AEC filter only converges while it is fed, so once started the feed and
 * fetch tasks run continuously — `jarvis_afe_set_capturing()` only decides
 * whether the enhanced audio is forwarded to the bridge, not whether the AFE
 * processes it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Initialise the AFE and start its feed/fetch tasks.
 *
 * Returns ESP_OK once the AFE is running. Returns ESP_ERR_NOT_SUPPORTED when
 * the firmware was built without the AFE (the caller should then keep using
 * the RMS silence gate), and another ESP_ERR_* code if the models or the AFE
 * could not be set up.
 *
 * Safe to call once, early in boot, before the WebSocket client is up.
 */
esp_err_t jarvis_afe_start(void);

/* True when jarvis_afe_start() succeeded and the AFE tasks are live. */
bool jarvis_afe_running(void);

/*
 * Hand playback PCM to the AEC as the far-end reference.
 *
 * Call this from the playback path with exactly the samples being written to
 * the codec, ideally just before the (blocking) codec write so the reference
 * leads the echo slightly. Samples are int16 mono at JARVIS_SAMPLE_RATE.
 *
 * Non-blocking. When the reference ring is full the oldest audio is dropped,
 * which at worst costs a little echo suppression until it re-syncs.
 */
void jarvis_afe_push_reference(const int16_t *pcm, size_t samples);

/*
 * Gate what is streamed to the bridge.
 *
 * While true, AFE-enhanced audio is sent to the bridge and the VAD endpoint
 * logic runs. While false nothing is sent, but the AFE keeps processing so the
 * echo canceller stays converged. Always safe to call; a no-op if the AFE is
 * not running.
 */
void jarvis_afe_set_capturing(bool on);

/*
 * Events raised from the AFE's fetch task. Handlers run on that task, so keep
 * them short and do not block (post to a queue / set state and return).
 */
typedef enum {
    /* The user finished speaking: VAD went speech -> silence while capturing.
     * This is the endpoint the turn machinery should act on. */
    JARVIS_AFE_EV_END_OF_SPEECH,
    /* Voice onset while NOT capturing — i.e. the user started talking over a
     * reply. Used for hands-free barge-in. Only raised after a quiet period,
     * so residual echo right at the start of playback does not trigger it. */
    JARVIS_AFE_EV_BARGE_IN,
} jarvis_afe_event_t;

typedef void (*jarvis_afe_event_cb_t)(jarvis_afe_event_t ev);
void jarvis_afe_set_event_cb(jarvis_afe_event_cb_t cb);

/*
 * Mic input level as an exponential average, 0..1 (1 = loud speech).
 * Mirrors jarvis_audio_mic_level() so the UI can poll one function regardless
 * of which capture path is active. Non-blocking.
 */
float jarvis_afe_mic_level(void);
