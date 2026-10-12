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

## Restart order / recovery

1. `systemctl start ollama` → wait for `ollama ps` (gpt-oss loaded).
2. `llmproxy` (auto-starts with ollama via `After=`); sanity: `curl -s -m 5 http://127.0.0.1:11440/v1/models`.
3. `hermes-gateway`; sanity: `curl -s -H "Authorization: Bearer $API_SERVER_KEY" http://127.0.0.1:8642/v1/models`.
4. `jarvis-voice`; sanity: ws_e2e_test with `/tmp/piper-test.wav`.

If a turn is mysteriously slow again: check `journalctl -u hermes-gateway` for `Auxiliary ... timed out`,
and `/var/log/llmproxy.log` for chat-completion timings; re-verify prefix caching in
`journalctl -u ollama` (`cached n_tokens ≈ total`).