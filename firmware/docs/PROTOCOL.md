# JARVIS bridge protocol (satellite ⇄ brain)

The satellite speaks a small JSON + binary WebSocket protocol to the JARVIS
brain (`jarvis_ai` voice bridge, running on the assistant host at
`ws://192.0.2.10:8765/ws`). This document is the authoritative contract for
the firmware side; it was derived from the deployed `bridge/server.py`.

* Transport: plain `ws://` (no TLS on this deployment).
* Text frames: UTF-8 JSON, one event per message.
* Binary frames: raw PCM audio, little-endian signed 16-bit, **16 kHz mono**.
* Text messages may be fragmented across WebSocket frames; the client must
  reassemble using `payload_len` / `payload_offset` before parsing
  (see `jarvis_ws.c`).

## Session outline

```
satellite                                   brain
   │  {"type":"start", ...}                  │
   │ ───────────────────────────────────────▶│   status: "Turn N recording started."
   │  <binary PCM frames …>                  │
   │ ───────────────────────────────────────▶│   partial_transcript (streaming)
   │  {"type":"stop"}                        │
   │ ───────────────────────────────────────▶│   transcript (final ASR)
   │                                         │   run_started
   │                                         │   agent_status: thinking / tool_use
   │  ◀────────────────────────────────────  │   <binary TTS PCM frames>
   │                                         │   agent_status: speaking
   │                                         │   done  (reply text in timing)
```

## Client → server

| Message | Payload | When |
|---|---|---|
| `start` | `{"type":"start","sample_rate":16000,"format":"pcm_s16le","channels":1,"conversation":"jarvis-main"}` | Begin an utterance. Follow with binary PCM. |
| binary | int16 LE, 16 kHz mono | Mic audio while listening. |
| `stop` | `{"type":"stop"}` | End the utterance; triggers ASR + agent. |
| `stop_run` | `{"type":"stop_run"}` | Barge-in / cancel the in-flight run (and TTS). |
| `approval_decision` | `{"type":"approval_decision","run_id":"…","approval_id":"…","decision":"allow"\|"deny"}` | Answer an `approval_request`. |

## Server → client

| Message | Fields | Meaning |
|---|---|---|
| `status` | `message` | Human-readable status; the greeting is `"Hermes voice server connected."`, and each turn starts with `"Turn N recording started."` |
| `partial_transcript` | `text` | Live, not-yet-final ASR text. |
| `transcript` | `text` | Final ASR text for the turn. |
| `run_started` | `run_id` | An agent run began; keep `run_id` for approvals. |
| `agent_status` | `state` (`thinking`/`tool_use`/`speaking`/`stopped`), `name` (on `tool_use`) | Drives the arc-reactor animation. |
| `approval_request` | `data` (object), `message` | The agent wants permission for a tool; reply with `approval_decision`. |
| `error` | `message` | Server-side error. |
| `done` | `turn_id`, `timing` | Turn complete. `timing` carries `response_text`, `transcript`, `tools_used`, `total_turn_seconds`. |
| binary | PCM | TTS audio for the reply (`response_text` is only guaranteed at `done`). |

### Notes / gotchas

* **The assistant's reply text arrives only in `done.timing.response_text`** —
  there is no separate "assistant message" event. The firmware therefore shows
  the reply *after* the audio has already started.
* `run_started.run_id` and `approval_request.data.id` (a.k.a. `approval_id`)
  must be echoed back on `approval_decision`. The firmware caches both
  (`jarvis_ws.c`) so the UI's simple allow/deny buttons can call
  `jarvis_turn_approval()` without threading ids through the UI.
* The server keeps its own keepalive; the client pings every 20 s with a 60 s
  pong timeout (mirrors `ws_ping_interval` in the server config).

## State mapping in the firmware

| Server signal | `jarvis_state_t` |
|---|---|
| local trigger accepted | `JVS_LISTENING` |
| `stop` sent | `JVS_THINKING` |
| `agent_status: thinking` / `tool_use` | `JVS_THINKING` |
| `agent_status: speaking` / first TTS binary | `JVS_SPEAKING` |
| `done` | `JVS_IDLE` (audio drains via `jarvis_audio_mark_done`) |
| WS disconnect / `error` | `JVS_ERROR` / reconnect |
