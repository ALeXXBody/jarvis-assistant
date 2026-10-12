/*
 * Wake / trigger sources.
 *
 * v1 triggers, in order of reliability on the bench:
 *   - tap the screen (GSL3680 touch)
 *   - serial console keys (l = listen, s = stop, x = abort, a/d = approval,
 *     +/- = volume, r = reconnect)
 *
 * A WakeNet ("Jarvis" wake word) hook is provided for later: stock ESP-SR
 * ships "Hi ESP" / "Alexa" models only, and a custom "Jarvis" wake word needs
 * an Espressif custom-model order. jarvis_wake_feed() is where a WakeNet task
 * would push mic samples; it is compiled out unless JARVIS_ENABLE_WAKENET is
 * defined.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t jarvis_wake_init(void);

/* Optional: feed mic PCM to the (future) wake-word engine.
 * Returns true if the wake word was detected. */
bool jarvis_wake_feed(const int16_t *pcm, size_t samples);
