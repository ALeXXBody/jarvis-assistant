# JARVIS — Voice Bridge (jarvis_ai) Deployment Notes

Live on the assistant host (`192.0.2.10`). Adapter of `eadmin2/jarvis_ai`
(`/opt/jarvis/jarvis_ai`) — satellite protocol + browser HUD, brain = Hermes.

## Services (all systemd, enabled at boot)

| Service | Port | Binary | Purpose |
|---|---|---|---|
| `ollama` | 127.0.0.1:11434 | system | gpt-oss:20b, 64K ctx (drop-in in `/etc/systemd/system/ollama.service.d/`) |
| `llmproxy` | 127.0.0.1:11440 | `/usr/bin/python3 /opt/jarvis/llmproxy.py 11440` | forces `reasoning_effort:none`/`think:false`; logs `/var/log/llmproxy.log` |
| `hermes-gateway` | 127.0.0.1:8642 | `/root/.local/bin/hermes gateway run` | Hermes API server (Bearer `API_SERVER_KEY` from `/root/.hermes/.env`) |
| `jarvis-voice` | 0.0.0.0:8765 | `/opt/jarvis/venv/bin/python3 server.py` (cwd `/opt/jarvis/jarvis_ai/server`) | voice WS `/ws` + HUD |

Key files: `/opt/jarvis/venv` (deps: faster-whisper, piper-tts, fastapi, uvicorn,
anthropic, …), `/opt/jarvis/voices/en_GB-alan-medium.onnx` (+`.json`),
`/opt/jarvis/jarvis_ai/server/config/server.yaml`, `~/.hermes/SOUL.md`.

## Satellite WS contract — `ws://192.0.2.10:8765/ws`

Client → server (JSON text / binary):
- `{"type":"start","sample_rate":16000,"format":"pcm_s16le","channels":1,"conversation":"jarvis-main"}` + binary int16 PCM chunks
- `{"type":"stop"}` — end speech, process turn · `{"type":"stop_run","run_id":...}` — halt agent · `{"type":"approval_decision",...}`

Server → client: `status`, `partial_transcript`, `transcript`, `run_started{run_id}`,
`agent_status{thinking|tool_use|speaking|stopped}`, `approval_request`, `error`,
`done{timing}` + **binary int16 PCM @16 kHz mono** TTS audio.

Test harness: `/opt/jarvis/jarvis_ai/server/scripts/ws_e2e_test.py <16k-mono.wav> <conversation> --realtime`

## Latency tuning (68 s → ≈5 s steady state)

- `agent.reasoning_effort: "none"` in `/root/.hermes/config.yaml` (kills gpt-oss thinking; ~8 tps gen remains).
- `llmproxy` forces reasoning off at the transport (Hermes doesn't send reasoning controls to local Ollama).
- `auxiliary.title_generation.enabled: false` + `model_upgrade_enabled: false` (was a hidden 30 s/session tax).
- `kanban.review_dispatch: false` (stops background review agents).
- Steady state: ~9.7k-token prompt prefix-caches in Ollama → prefill ~0.7 s; gen 3–8 s.
- First turn after idle: ~23 s tail prefill. Tool loops (agentic turns): 60–120 s — tuning lever for later
  (fast small model for voice, big model escalation, or real Home Assistant toolset).

## Streaming & spoken-ack tuning (this repo's `bridge/server.py`)

Two knobs shape how a reply reaches the ear. Both are in `config/server.yaml`
(`voice:` / `hermes:` blocks) and both degrade to the old behaviour if unset.

- **`voice.min_chunk_chars`** (default `60`) — the reply is synthesized in
  clauses, not whole sentences. A chunk always ends at `.!?`; it may also end at
  `,;:` once it has reached this many characters. This is what makes the first
  words audible sooner: an agent sentence can run to 300 characters, and waiting
  for its full stop costs a second of silence before any audio. Every chunk pays
  Piper's startup, so going much below ~40 makes the reply choppy and no faster
  overall. `0` restores full-sentence-only splitting.
- **`hermes.ack_after_seconds`** (default `0` = off) + **`ack_texts`** — if no
  reply sentence has started synthesizing after this many seconds, speak the next
  ack text as filler (rotating through the list across turns). The ack is
  cancelled the moment the real reply is ready, and skipped entirely when the
  agent answers quickly, so a snappy turn is never prefixed with filler. Useful
  range is ~1.5–2.5 s. The ack's audio is written under `conn.tts_lock` and the
  sentence loop waits on that lock, so the two PCM streams never interleave
  (two overlapping streams arrive as noise, not two voices). Ack metrics are
  kept out of the turn's latency numbers.

The chunker and ack task have stdlib-only tests — run them anywhere the repo is
checked out, no venv or models needed:

```
python3 bridge/tests/test_chunker.py
python3 bridge/tests/test_ack.py
```

**Deploying:** the live host runs a *copy* at
`/opt/jarvis/jarvis_ai/server/server.py` with its own
`config/server.yaml`. Copy both files across (or re-apply the two knobs by hand)
and restart `jarvis-voice`; the ack stays off until `ack_after_seconds > 0`.

## Restart order / recovery

1. `systemctl start ollama` → wait for `ollama ps` (gpt-oss loaded).
2. `llmproxy` (auto-starts with ollama via `After=`); sanity: `curl -s -m 5 http://127.0.0.1:11440/v1/models`.
3. `hermes-gateway`; sanity: `curl -s -H "Authorization: Bearer $API_SERVER_KEY" http://127.0.0.1:8642/v1/models`.
4. `jarvis-voice`; sanity: ws_e2e_test with `/tmp/piper-test.wav`.

If a turn is mysteriously slow again: check `journalctl -u hermes-gateway` for `Auxiliary ... timed out`,
and `/var/log/llmproxy.log` for chat-completion timings; re-verify prefix caching in
`journalctl -u ollama` (`cached n_tokens ≈ total`).