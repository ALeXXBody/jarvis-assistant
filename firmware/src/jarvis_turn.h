/*
 * Turn control — the small state machine that ties the trigger, the mic and
 * the bridge together.
 *
 *   start  : state=LISTENING, ws "start", begin mic streaming
 *   stop   : stop mic, ws "stop", state=THINKING
 *   abort  : ws "stop_run", flush playback, state=IDLE (barge-in / cancel)
 *   toggle : what a tap / button / wake word calls
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

void jarvis_turn_init(void);

/* Begin a new utterance (no-op if not in a startable state). */
void jarvis_turn_start_listening(void);

/* End the utterance and hand it to the server. */
void jarvis_turn_stop_listening(void);

/* Cancel the in-flight turn (barge-in) and return to idle. */
void jarvis_turn_abort(void);

/* Tap/button/wake-word behaviour: start listening, or stop if already on. */
void jarvis_turn_toggle(void);

/* Answer a pending approval prompt. */
void jarvis_turn_approval(bool allow);

/* Reset to idle (e.g. after a disconnect). */
void jarvis_turn_reset(void);
