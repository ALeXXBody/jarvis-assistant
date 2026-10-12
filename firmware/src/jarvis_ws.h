/*
 * WebSocket client for the JARVIS bridge protocol (ws://host:port/ws).
 *
 * Wire contract (see BRIDGE_NOTES.md, bridge/server.py):
 *   client -> server : {"type":"start",...} + binary PCM 16 kHz mono
 *                      {"type":"stop"} / {"type":"stop_run"}
 *                      {"type":"approval_decision","run_id":..,"approval_id":..,
 *                       "decision":"allow"|"deny"}
 *   server -> client : text JSON events (status, partial_transcript, transcript,
 *                      run_started, agent_status, approval_request, error, done)
 *                      binary frames = TTS PCM (16 kHz mono S16LE)
 *
 * Text frames may be fragmented across events; we reassemble via
 * payload_offset/payload_len. Binary frames stream straight to playback.
 *
 * All inbound handling runs on the esp_websocket_client task; outbound helpers
 * may be called from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Creates the client and starts the connect/reconnect loop. */
esp_err_t jarvis_ws_start(const char *url);

/* Stops and destroys the client. */
void jarvis_ws_stop(void);

bool jarvis_ws_connected(void);

/* --- outbound (call from any task) -------------------------------------- */
esp_err_t jarvis_ws_send_start(const char *conversation);
esp_err_t jarvis_ws_send_audio(const void *data, size_t len);
esp_err_t jarvis_ws_send_stop(void);
esp_err_t jarvis_ws_send_stop_run(void);
/* Pass NULL ids to use the ids captured from the last approval_request. */
esp_err_t jarvis_ws_send_approval(const char *run_id, const char *approval_id, bool allow);
