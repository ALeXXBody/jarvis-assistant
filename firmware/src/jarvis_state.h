/*
 * Central application state + a snapshot for the UI.
 *
 * Thin, mutex-protected store so the WebSocket task, audio task and UI task
 * can share state without tearing. The UI polls jarvis_state_copy() at frame
 * rate; nothing here allocates.
 */
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

typedef enum {
    JVS_BOOTING = 0,
    JVS_IDLE,        /* connected, waiting for a trigger            */
    JVS_LISTENING,   /* mic streaming to the server                  */
    JVS_THINKING,    /* utterance sent, ASR + agent running          */
    JVS_SPEAKING,    /* TTS audio arriving/playing                   */
    JVS_ERROR,       /* disconnected / fault                          */
} jarvis_state_t;

#define JVS_LINE_MAX 96
#define JVS_LINES     5

typedef struct {
    jarvis_state_t state;
    bool  wifi_connected;
    bool  ws_connected;
    bool  approval_pending;
    char  status[112];            /* last server status line            */
    char  tool[48];               /* last tool_use name                 */
    char  user_text[224];         /* last final ASR transcript          */
    char  reply_text[480];        /* last assistant reply text          */
    char  error[128];             /* last error                         */
    float last_total_seconds;     /* latency of last turn (done.timing) */
    int   battery_pct;            /* -1 = unknown                       */
    char  lines[JVS_LINES][JVS_LINE_MAX];  /* rolling display lines     */
    TickType_t tap_ticks;         /* freshly stamped when a tap lands   */
} jarvis_snapshot_t;

void jarvis_state_init(void);

void jarvis_state_set(jarvis_state_t s);
jarvis_state_t jarvis_state_get(void);
const char *jarvis_state_name(jarvis_state_t s);

/* Copies the full snapshot under lock (safe from any task). */
void jarvis_state_copy(jarvis_snapshot_t *out);

void jarvis_state_status(const char *msg);
void jarvis_state_tool(const char *name);
/* Live (partial) ASR text — shown while the utterance is still open. */
void jarvis_state_partial(const char *text);
/* Final ASR text for the turn. */
void jarvis_state_transcript(const char *text);
void jarvis_state_reply(const char *text);
void jarvis_state_error(const char *msg);
void jarvis_state_conn(bool wifi, bool ws);
void jarvis_state_approval(bool pending, const char *desc);
void jarvis_state_metrics(float total_seconds);
void jarvis_state_set_battery(int pct);

/* Appends a line to the rolling on-screen transcript (oldest scrolls off). */
void jarvis_state_push_line(const char *line);

/* Wake/touch task: stamp "a tap just landed" — the UI draws a ripple. */
void jarvis_state_touch_pulse(void);
