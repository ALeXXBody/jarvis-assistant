# JARVIS-Style Personal Assistant on the GUITION JC8012P4A1C 10.1" Panel
### Research & full setup plan

> **Board:** GUITION **JC8012P4A1C_I_W_Y** (Jingcai Intelligent / Guition) — 10.1", 800×1280 IPS, capacitive touch, ESP32‑P4 + ESP32‑C6
> **Official archive:** `JC8012P4A1C_I_W_Y.zip` (~402 MB, 9 shards) — spec sheet, schematic, demos, tools (being inventoried)
> **Goal:** a wake‑word voice assistant with a JARVIS‑style animated UI on the panel.

---

## 1. TL;DR — what a "full setup" actually consists of

A JARVIS assistant on this panel is **three systems that must all exist**:

| # | System | Runs on | What it does |
|---|--------|---------|--------------|
| 1 | **Firmware** (edge client) | the panel (ESP32‑P4 + C6) | mic capture, offline wake word, audio codec, screen/LVGL UI, WiFi, streaming to server |
| 2 | **Assistant server** (the brain) | your home server `192.0.2.20` | ASR → LLM → TTS, tools/MCP, memory, Home Assistant bridge |
| 3 | **Integrations** | LAN | Home Assistant / MQTT / media server / calendar / weather / custom tools |

The panel **cannot** run the LLM itself. It is a thin audio+display client; the intelligence lives on your server. This is exactly the design of the open‑source **Xiaozhi‑ESP32** project (小智), which this exact panel is *advertised as supporting* (AliExpress listing: *"supports Xiaozhi ESP32 C6"*).

**Two viable paths:**

- **Fast path** — flash the open‑source **xiaozhi‑esp32** firmware (ESP‑IDF 6 + ESP‑SR) and self‑host **xiaozhi‑esp32‑server** on the home server. Least custom code.
- **Chosen path (this build)** — write **custom ESP‑IDF firmware** so the panel is a **bespoke "hi‑tech satellite"** (own UI, own protocol, own behaviour), and run the brain on the **home server**. See **§6.5** for the full architecture and **§6.6** for prior art. You pay in firmware effort; you gain total control of the satellite. For the brain, **reuse a proven local‑first project (OpenJarvis / xiaozhi server) instead of writing the ASR/LLM/TTS/tools glue yourself**.

> **Environment note:** the assistant runs on a **home server** on the LAN
> with **ample CPU and RAM** and a local **Ollama** already serving several
> models. There is **no Docker socket available** → run the server natively
> (Python venv + systemd), or deploy it via the host's Apps system. This is
> more than enough to host the whole assistant locally.

---

## 2. The hardware you have

### 2.1 Core specs (from Guition's official EN spec sheet)

| Parameter | Value |
|---|---|
| Module | GUITION JC8012P4A1C_I_W_Y |
| SoC | **ESP32‑P4** dual‑core RISC‑V, up to **360–400 MHz** |
| Memory | 768 KB HP L2MEM, 32 KB LP SRAM, 128 KB HP ROM |
| PSRAM | **32 MB** (stacked) |
| Flash | **16 MB** NOR (W25Q128) — *some reseller listings say 8 MB PSRAM; verify your unit* |
| Wireless | **ESP32‑C6‑MINI‑1** (Wi‑Fi 6 + BT 5 LE) via **ESP‑Hosted over SDIO** (the P4 has no radio) |
| Display | 10.1" IPS TFT, **800 × 1280** native portrait (=1280×800 landscape), **JD9365** driver, 2‑lane **MIPI‑DSI** |
| Touch | **GSL3680 / Silead** capacitive, I²C addr `0x40` |
| Audio | **ES8311** mono codec (I²S + I²C) + onboard mic + speaker amp (PA enable) |
| Camera | **MIPI‑CSI** connector (2 MP camera supported) |
| Storage | microSD (TF) slot, 4‑bit SDMMC |
| Other | RTC (RX8025T) + battery holder, WS2812 RGB LED, 3.7 V Li‑ion connector + power button |
| Power | **5 V DC, ~700 mA** (use a ≥2 A supply) |
| Size | Display 216.58 × 135.36 mm; module 242.80 × 158.70 mm |
| Temp | −20 °C … 70 °C operating |
| Dev support | ESP‑IDF, Arduino, MicroPython, Guition Designer |

### 2.2 Key pin map (community‑verified / BSP)

```
Display   JD9365 MIPI-DSI 2-lane, reset GPIO27, backlight GPIO23 (LEDC/PWM, active-high)
Touch     GSL3680  SDA GPIO7  SCL GPIO8  RESET GPIO22  INT GPIO21   (I2C 0x40)
Audio     ES8311   MCLK GPIO13  BCLK GPIO12  LRCLK GPIO10  DOUT GPIO9  DIN GPIO11  PA_CTRL GPIO20
Wi-Fi/C6  SDIO    CLK18 CMD19 D0=14 D1=15 D2=16 D3=17  C6 reset GPIO54
microSD   CLK43 CMD44 D0=39 D1=40 D2=41 D3=42
Battery   ADC GPIO52 (divider ×1.68)
```

### 2.3 What the panel is good at / not good at

**Good:** large 1280×800 canvas (great for a JARVIS orb/radar dashboard), hardware‑accelerated graphics (PPA + LVGL 9), H.264 encode, camera input, RTC, battery option, TF card for local audio/assets.

**Limits:** no native Wi‑Fi (bridged via C6 + ESP‑Hosted — adds complexity), P4 audio is a **single ES8311 mono codec + onboard mic** → *not* a far‑field mic array, so wake‑word range/accuracy is modest. For a JARVIS across a room you may later add a better mic (e.g. external I²S mic array) or a satellite.

---

## 3. What the official archive contains

`JC8012P4A1C_I_W_Y.zip` is the vendor package. Typically for Guition JC‑series it includes:

- **Datasheet / specifications PDF** (EN, 7 pages — already retrieved)
- **Schematic** PDF (full board design)
- **3D / dimension drawings**
- **Factory test + demo firmware** (LVGL demo, touch/display test)
- **ESP‑IDF / Arduino example projects** and possibly **Guition Designer** project files
- **USB/serial drivers, flash tools**

> The archive is 402 MB / 9 shards and downloads slowly from the vendor CDN (~50–130 KB/s). It is being pulled in the background; this section will be replaced with the **actual file inventory** once extracted.

---

## 4. Functional requirements of a "JARVIS" assistant

Minimum lovable product:

1. **Wake word** — always‑on, offline detection (say "Jarvis").
2. **Speech‑to‑text (ASR)** — streaming, low latency.
3. **LLM** — conversational brain with a *Jarvis* personality + tool calling.
4. **Tools / actions** — control home, play media, answer queries, run routines (MCP).
5. **Text‑to‑speech (TTS)** — natural voice reply.
6. **Animated UI** — idle/listening/thinking/speaking states, an arc‑reactor/orb look.
7. **Optional:** vision (camera), presence, memory, multi‑room satellites.

---

## 5. Architecture options

### Option A — Xiaozhi‑ESP32 + self‑hosted server  ⭐ recommended
Matches the hardware exactly; least custom firmware. Offline wake word (ESP‑SR) on the P4, streaming ASR/LLM/TTS on your server, MCP tool calling, LVGL emoji/UI support, 138+ board profiles.

```
[Panel: xiaozhi-esp32 firmware]
   mic ▸ Opus over WebSocket/MQTT ▸ [Server: xiaozhi-esp32-server]
                                        ├─ ASR  (FunASR / Whisper / cloud)
                                        ├─ LLM  (Ollama Qwen / OpenAI‑compatible / DeepSeek)
                                        ├─ TTS  (EdgeTTS / Piper / FishSpeech / GPT‑SoVITS)
                                        └─ MCP tools ▸ Home Assistant, MQTT, APIs
   speaker ◂ Opus stream ◂ ────────────┘
   screen  ◂ state/emoji from firmware ◂
```

### Option B — Home Assistant "Assist" + Wyoming satellites
Use HA's native voice pipeline (`Whisper` STT, `Piper` TTS, `openWakeWord`, conversation agent = Ollama or OpenAI). Excellent if your home is already Home Assistant‑centric. **Downside:** ESPHome support for this P4/MIPI‑DSI panel is still bleeding‑edge/buggy; and HA's openWakeWord runs server‑side, not on the panel.

### Option C — Hybrid / "true JARVIS" thin client
Panel runs a custom LVGL UI and streams audio to a richer agent framework on the server (LangGraph / custom orchestrator + Home Assistant + long‑term memory + vision). Most work, most control, best long‑term ceiling.

**Recommendation:** Start with **A** (fastest to working), keep the door open to **C** by putting the brain on the server from day one.

---

## 6. The stack, component by component

| Layer | Recommended default | Local / privacy option | Cloud option |
|---|---|---|---|
| **Firmware** | xiaozhi‑esp32 (ESP‑IDF 6, ESP‑SR 2.4.7) | same | same |
| **Wake word** | **ESP‑SR WakeNet** on‑device (offline) | WakeNet; or custom‑trained "Jarvis" model | — |
| **ASR** | **faster‑whisper** (large‑v3 / distil) | FunASR / Sherpa‑ONNX / whisper.cpp | OpenAI / Deepgram |
| **LLM** | **Ollama + Qwen2.5‑7B‑Instruct** (fits the home server) | any local GGUF | OpenAI / Anthropic / DeepSeek / Qwen |
| **TTS** | **Piper** (fast, CPU) or **EdgeTTS** (very natural, free) | Piper / Coqui / GPT‑SoVITS | Azure / ElevenLabs |
| **Glue** | **xiaozhi‑esp32‑server** (Docker) | same | xiaozhi.me cloud |
| **Tools** | **MCP** (device + cloud) | custom functions | MCP servers |
| **Home** | **Home Assistant** + MCP integration | fully local | — |
| **UI** | LVGL 9 + PPA (custom JARVIS theme) | — | — |

**Wake‑word notes**
- On‑device wake word = **ESP‑SR "WakeNet"**. Espressif ships some wake words free; a **custom "Jarvis" wake word needs Espressif's customization service** (fee depends on volume).
- Alternatives if you don't want to pay/wait: use a shipped wake word (e.g. "Hi ESP"/"你好小智"), or **train an openWakeWord / microWakeWord "hey jarvis" model** (openWakeWord already has a *hey_jarvis* model) — but note that path is designed for Wyoming/microWakeWord (typically ESP32‑S3 + ESPHome), so it's more relevant to Option B/C than to ESP‑SR.

**Server sizing (xiaozhi‑esp32‑server docs)**
- Minimal, API‑only ASR/LLM/TTS: ~**2 cores / 2 GB**
- With **local FunASR**: ~**2 cores / 4 GB**
- Full module install (multi‑user, agents, dashboard, FunASR): ~**4 cores / 8 GB**
- Prefer a GPU for local Whisper/Ollama; CPU‑only works for small models but with higher latency.

---

## 6.5 Custom satellite architecture (chosen direction)

The panel runs **bespoke firmware** and behaves as a "hi‑tech satellite": it owns audio, wake word, and a rich JARVIS UI; the **home server owns all intelligence**. The two talk over a small, explicitly‑defined protocol so both sides can be built to match.

### 6.5.1 Block diagram

```
┌─────────────────────── JARVIS PANEL  (ESP32-P4 + ESP32-C6) ──────────────────────┐
│  custom ESP-IDF 6 firmware                                                        │
│                                                                                   │
│  wake/   ESP-SR WakeNet ("Jarvis") + VAD       ui/    LVGL 9 + PPA                │
│  audio/  ES8311 mic → Opus enc │ Opus dec → ES8311 + PA   (orb, states, text)     │
│  bsp/    JD9365 DSI, GSL3680 touch, C6 Wi-Fi (esp-hosted), CSI cam, WS2812, RTC  │
│  net/    WebSocket client  ──────── Opus/JSON ────────┐   state machine + LED     │
└───────────────────────────────────────────────────────┼──────────────────────────┘
                                                         │  ws://<server>:8090/jarvis/v1
┌────────────────────────────────────────────────────────▼──────────────────────────┐
│  JARVIS SERVER  (Python, native in home-server container / LXC)                   │
│                                                                                   │
│  gateway/   FastAPI + websockets · sessions · protocol · barge-in                 │
│  asr/       faster-whisper (large-v3 / distil)  ← CPU real-time OK                │
│  llm/       Ollama @127.0.0.1:11434  (qwen3:8b fast · qwen3.6:35b-a3b smart)     │
│  tts/       Piper (fast) or EdgeTTS (natural) → Opus                              │
│  tools/     MCP client → Home Assistant · media · weather · calendar · time       │
│  memory/    SQLite + embeddings (conversation memory, RAG optional)               │
└───────────────────────────────────────────────────────────────────────────────────┘
```

### 6.5.2 Satellite ⇄ server protocol (v1)

One WebSocket per device. **Text frames = JSON control/events; binary frames = Opus audio.**

**Device → server (JSON)**
```json
{"type":"hello","device":{"id":"jarvis-panel","fw":"0.1","in_rate":16000,"channels":1},"auth":"<token>"}
{"type":"listen.state","state":"start|stop"}     // wake word / VAD gate
{"type":"abort"}                                  // barge-in: stop current TTS
{"type":"ping"}
```

**Device → server (binary)** — Opus packets of microphone audio (while listening).

**Server → device (JSON)**
```json
{"type":"hello.ack","session":"...","out_rate":24000}
{"type":"state","value":"idle|listening|thinking|speaking"}   // drives the UI + LED
{"type":"asr.partial","text":"..."} / {"type":"asr.final","text":"..."}
{"type":"llm.delta","text":"..."}  / {"type":"llm.final","text":"..."}
{"type":"tool","name":"home.light.set","args":{...},"status":"ok"}
{"type":"emotion","value":"neutral|happy|alert|thinking"}
{"type":"tts.start"} / {"type":"tts.stop"}
{"type":"error","message":"..."}
```

**Server → device (binary)** — Opus packets of synthesized speech.

**Turn flow:** idle → (on‑device wake word) → `listen.state:start` + mic stream → server VAD+ASR → `asr.final` → LLM (+tools/memory) → stream `llm.delta` to screen → TTS → Opus back → playback → `tts.stop` → idle. Barge‑in: wake word during playback → `abort`.

> **Design shortcut:** this protocol is deliberately close to xiaozhi's, so you can *either* keep it bespoke *or* later swap in `xiaozhi-esp32-server` (which already implements ASR/LLM/TTS/MCP) with minimal adapter work. Reuse the mature plumbing, keep the custom satellite.

### 6.5.3 Custom firmware — module breakdown (ESP‑IDF components)

| Component | Responsibility | Key notes |
|---|---|---|
| `bsp` | board bring‑up | JD9365 DSI init sequence + timing (60 MHz pclk, 1 Gbps/lane, hsync back20/pulse20/front40, vsync back8/pulse4/front20), GSL3680 I²C touch, ES8311 I²S, C6 via `esp_wifi_remote`/esp‑hosted, MIPI‑CSI camera, WS2812 (RMT), RX8025T RTC, battery ADC GPIO52 |
| `audio` | duplex I²S | ES8311 config, PA enable GPIO20, 16 kHz mic in, 24 kHz out, **libopus** encode/decode, jitter buffer |
| `wake` | always‑on trigger | **ESP‑SR WakeNet** + VAD; slot for a custom "Jarvis" model |
| `ui` | JARVIS visuals | **LVGL 9 + PPA**: idle arc‑reactor orb, listening pulse, thinking spinner, speaking waveform, transcript view, status bar (Wi‑Fi/RTC/battery). Bind to `state`/`emotion` messages |
| `net` | connectivity | Wi‑Fi provisioning (SoftAP/BLE), WebSocket client (optional TLS), auto‑reconnect, JSON control channel |
| `app` | orchestration | state machine (idle→listening→thinking→speaking), display + WS2812 sync, button/touch gestures, OTA |
| `config` | settings | NVS: SSID/pass, server URL, auth token, wake‑word, personality/volume |

### 6.5.4 Server — run it natively on the home server

Because there's no Docker socket inside the dev container, run the server as **native services** (or via the host's Apps UI if you prefer containers):

```
/opt/jarvis/
  venv/                     python3 -m venv
  server/                   gateway (FastAPI+uvicorn), asr, llm, tts, tools, memory
  models/piper/             piper voice .onnx
  systemd: jarvis-server.service  (Restart=always)
Ollama: already running at 127.0.0.1:11434 (reuse it)
```

- **ASR:** `faster-whisper` (`large-v3` int8) — real‑time on CPU with many cores; or `distil-large-v3` for lower latency.
- **LLM:** point at existing Ollama — `qwen3:8b`/`qwen3-fast` for latency, `qwen3.6:35b-a3b` (MoE, vision+tools) for quality; tool calling built in.
- **TTS:** `piper` (CPU, very fast) or `edge-tts` (natural, free); optionally a JARVIS‑style voice clone later.
- **Tools:** an MCP client wiring Home Assistant, media server (`192.0.2.40:8096`), media-requests (`:5055`), weather, calendar, timers.
- **Memory:** SQLite for conversation history; embeddings via Ollama for RAG later.

### 6.5.5 Phased build (custom path)

1. **Bring‑up** — ESP‑IDF 6 project; display + touch + ES8311 loopback + Wi‑Fi via C6.
2. **Protocol skeleton** — WS echo server (Python) + firmware client; JSON `state` round‑trip drives a coloured dot on screen.
3. **Audio loop** — mic → Opus → server → decode → speaker (walkie‑talkie test).
4. **Wake word** — ESP‑SR on device gates the stream; UI shows *listening*.
5. **ASR → LLM → TTS** — full voice turn; transcript + `state` on the orb.
6. **JARVIS UI** — animate idle/listening/thinking/speaking; WS2812 reactor glow.
7. **Tools** — MCP to Home Assistant / media / weather; render tool activity on screen.
8. **Polish** — barge‑in, memory, camera vision, custom "Jarvis" wake word, OTA.

---

## 6.6 Prior art — the best JARVIS projects (and what to take)

Searched GitHub + web (Oct 2026). Star counts are live.

| Repo | ★ | What it is | What to take |
|---|---|---|---|
| **open-jarvis/OpenJarvis** | **11.0k** | Stanford research framework: *"Personal AI, On Personal Devices."* Composable, local‑first. | **The best model for our server brain.** Has `speech/` (faster‑whisper, Kokoro TTS, Deepgram, OpenAI TTS), `mcp/` (client+server), `memory/` (extractor/service/store), `skills/`, `connectors/`, `channels/`, and a FastAPI `server/` with **`ws_bridge` + `stream_bridge`**. Engine adapters include **Ollama**. Fast/slow **model routing**. |
| **leon-ai/leon** | 17.6k | Long‑standing privacy‑first personal assistant. Intents‑first NLU + skills. | Skill/intent architecture (fast deterministic handling for commands, LLM only when needed); 100 %‑offline mindset. |
| **microsoft/JARVIS** (HuggingGPT) | 25.4k | Research: an LLM **orchestrating** specialist models/tools. | The "LLM as planner/router" pattern — our tool‑calling layer generalises this. |
| **isair/jarvis** | 2.0k | Local‑first **voice** assistant: Whisper + Ollama/OpenAI + TTS, private. | **The UX blueprint:** "a third person in the room", address by name *anywhere* in a sentence, a short rolling transcript of nearby speech, personal **diary/memory**, secret **redaction before context**, MCP tools. |
| **Priler/jarvis** | 3.0k | Offline voice assistant in **Rust/Tauri** (Vosk, Porcupine/rustpotter). | Wake‑word options beyond ESP‑SR; fully‑offline posture. |
| **AlexandreSajus/JARVIS** | ~0.5k | Minimal Voice→Text→LLM→Speech pipeline + web UI. | Clean reference for the audio pipeline + on‑screen transcript. |
| **ethanplusai/jarvis** | 0.9k | Voice assistant for Claude Code. | Barge‑in / conversational coding‑agent UX. |
| **xiaozhi-esp32 / ‑server** | ~big | ESP32 voice‑assistant ecosystem (P4 supported). | The satellite↔server protocol + a ready ASR/LLM/TTS/MCP server we can borrow from. |
| **Home Assistant voice + Wyoming** | (HA core 91k) | Production on‑device wake + streaming ASR satellites. | The wake→stream→TTS **satellite pattern** and MCP‑style device control. |

### Patterns every good one shares
1. **Wake word on the device, brain off the device.** (Our exact plan.)
2. **Streaming at every stage** — partial ASR, token‑by‑token LLM, chunked TTS. Perceived latency matters more than total.
3. **Memory that persists** — a diary/vector store that survives sessions (isair: "memory that doesn't reset"; OpenJarvis `memory/`).
4. **Tools via a standard layer (MCP)** — not hard‑coded integrations.
5. **Fast/slow model split** — a small model for routing/intent, a big one for depth (OpenJarvis routing; Leon intents).
6. **Local‑first, privacy‑first** — redact secrets before they touch context; everything on your own hardware.
7. **A visible presence** — transcript/state on screen so you can *watch* it think (our LVGL orb).

### How this maps onto our build
- **The server is largely a solved problem.** OpenJarvis already ships faster‑whisper + Kokoro TTS + Ollama + MCP + memory + a FastAPI WebSocket server. The pragmatic move is to **run OpenJarvis (or lift its `speech/` + `memory/` + `mcp/` + `server/ws_bridge` pieces) as the brain on the home server**, and keep our effort where the value is: the **custom ESP32‑P4 satellite + JARVIS UI** + the small device protocol in §6.5.2.
- **The firmware side has no equivalent** — this is where our custom work is genuinely needed (P4 board bring‑up, wake word, LVGL arc‑reactor UI). That's the differentiator.
- **Borrow the UX**: name‑in‑sentence addressing, rolling transcript, persistent memory/diary, redaction, on‑screen "thinking" state.

> **Revised recommendation:** build the **custom satellite firmware** (our differentiating work) and **reuse a proven brain** rather than writing ASR/LLM/TTS glue from scratch — either **OpenJarvis** or the **xiaozhi server**. Fall back to the thin custom server in §6.5.4 only if those don't fit the device protocol.

---

## 6.7 The J.A.R.V.I.S. spec — what "Marvel J.A.R.V.I.S." actually is (our target)

> Reading "Marvell" as **Marvel's J.A.R.V.I.S.** No notable *Marvell* project exists on GitHub; this section fixes the target so we build toward the right thing.

**Canon (MCU / Marvel Comics):** J.A.R.V.I.S. — *"Just A Rather Very Intelligent System"* — is the AI Tony Stark built. He is voiced by **Paul Bettany**. He starts out as a **natural-language UI** and grows into the system that runs Stark's **house**, the **Iron Man suits**, the **Iron Legion**, and much of **Stark Industries**. Tony, to Bruce Banner: *"Started out, J.A.R.V.I.S. was just a natural language UI. Now he runs the Iron Legion. He runs more of the business than anyone besides Pepper."* Personality: a **cultured British butler** — composed, dry-witted, loyal, deferential but candid. (In *Age of Ultron* he is uploaded into **Vision**; **F.R.I.D.A.Y.** takes over.)

### Capability → what it means for us

| J.A.R.V.I.S. capability (canon) | How it manifests | Our buildable version | v1? |
|---|---|---|---|
| **Natural-language interface** | Talk naturally, follow-ups, no rigid syntax | wake word → ASR → LLM(+memory) → TTS | ✅ core |
| **"Runs the house"** | lights, climate, media, locks, security, displays | **Home Assistant via MCP**; media via the media server | ✅ v1 |
| **Presence & voice** | British baritone + holographic UI | TTS voice choice + **LVGL arc-reactor orb** | ✅ core |
| **Perception / awareness** | cameras, sensors; IDs people & threats; vitals | MIPI-CSI cam → **vision LLM** (`qwen3.6:35b-a3b`, already local) | 🟡 later |
| **Engineering aid** | simulate, calculate, diagnose, design | LLM + code/calculator tools; panel as a work surface | 🟡 partial |
| **"Runs the business"** | schedule, comms, briefings, ops | calendar/email connectors, **morning brief** | 🟡 partial |
| **Security** | biometric protocols, locks, Iron Legion | HA locks + device auth; access control | 🟡 later |
| **Ambient / multi-device** | in the house, suits, car, phone | extra satellites, phone/browser client | 🟡 later |
| **Proactivity** | warns, suggests, monitors unprompted | scheduled + event triggers (OpenJarvis proactive agent) | 🟡 later |
| **Suits / flight control** | fly the armor, HUD, targeting | control real smart devices/robots | ⚪ aspirational |
| **True AGI reasoning** | solve anything, create | the ongoing frontier | ⚪ fiction |

### What makes it J.A.R.V.I.S. and not Alexa
1. **Address by name anywhere** in a sentence; no rigid command grammar.
2. **Concise, confident, dry-witted** replies (a *butler*, not a chatbot).
3. **Always-on ambient awareness** — he speaks up when it matters.
4. **A visible presence** — the arc-reactor / holographic interface.
5. **Trustworthy** — runs your life, keeps your secrets; ours stays **local/private**.

### The maturity ladder (so we know what "done" means)
- **L0** — voice commands (Siri/Alexa class)
- **L1** — conversational + remembers you
- **L2** — *acts* in your home/world via tools ← **our v1 target**
- **L3** — proactive, always-on awareness (watches, warns, suggests)
- **L4** — ambient multi-device presence (every room, phone, car)
- **L5** — the fiction (autonomous, creative, runs your life)

> **Build L2 completely; design the protocol so L3 is a drop-in.**

### Voice spec
- Target: **British RP, male baritone, calm, articulate** (a "butler" register).
- Options: **Piper** (CPU-fast, slightly robotic) · **Coqui XTTS / voice-clone** (closest, local) · **ElevenLabs** (best, cloud).
- ⚠️ Paul Bettany's performance is a copyrighted voice — build a **"cultured British butler"** voice, **do not clone the actor** for anything you share publicly.

---

## 6.8 Hermes Agent — the brain people are *actually* using (revised recommendation)

> You flagged it and the ecosystem agrees: the JARVIS builds that work are **Hermes Agent** front-ends. This supersedes the OpenJarvis lean in §6.6.

### What Hermes Agent is
`NousResearch/hermes-agent` — **~253k★, MIT, actively developed (commits daily)** — *"the agent that grows with you."* It is a full **autonomous agent**, not a chatbot wrapper:

| Capability | Detail |
|---|---|
| **Tools** | 60+ built-ins: terminal, files, web search, browser, computer-use, vision, image gen, TTS, Spotify, email |
| **Memory** | persistent `MEMORY.md`/`USER.md`, FTS5 cross-session search + LLM summarization, pluggable providers (Honcho, Mem0, Hindsight, Holographic, …) |
| **Skills** | agent **creates and self-improves skills**; agentskills.io open standard |
| **Proactivity** | built-in **cron** scheduler + automations + `hermes send` to any platform |
| **Gateway** | **20+ messaging platforms**: Telegram, Discord, Slack, WhatsApp, Signal, Matrix, SMS, Email, **Home Assistant**, … |
| **Voice** | full **Voice Mode** (see below) + built-in **Wake Word** |
| **MCP** | connect any MCP server |
| **Persona** | global **SOUL.md** + built-in personalities + Bot Mode (named agents) |
| **Backends** | local, Docker, SSH, Singularity, Modal, Daytona, Vercel Sandbox |
| **Models** | any OpenAI-compatible endpoint — incl. **local Ollama** — plus Nous Portal/OpenRouter/OpenAI/Anthropic/Gemini/… |
| **Install** | `curl -fsSL https://hermes-agent.nousresearch.com/install.sh \| bash` (PM-managed Python 3.14; data in `~/.hermes/`) |
| **Admin** | Hermes **web dashboard** (config, keys, MCP, gateway, memory, cron, logs, analytics) |

### Why it maps 1:1 onto the §6.7 JARVIS spec

| J.A.R.V.I.S. need | Hermes feature — for free |
|---|---|
| "Runs the house" | **Home Assistant plugin** + MCP |
| Remembers you | memory + user profile (closed learning loop) |
| Talks & listens | **Voice Mode**: local `faster-whisper` STT (no key), sentence-by-sentence streaming TTS, **barge-in**, hallucination filter |
| Free/local voice | TTS providers **Piper, NeuTTS, Edge — all local, zero key** (+ ElevenLabs/OpenAI/xAI optional) |
| Wake word | built-in **Wake Word** ("Hey Hermes" or custom) |
| Personality | **SOUL.md** butler persona |
| Proactivity (L3) | **cron** + event triggers + `hermes send` |
| Multi-device (L4) | 20+ chat surfaces; the panel becomes just another face |
| Private & local | Ollama + local STT/TTS ⇒ **no cloud required** |
| Does real work | 60+ tools, 7 execution backends |

### The proven JARVIS-on-Hermes pattern (reuse, don't reinvent)
- **`eadmin2/jarvis_ai`** (MIT) — *"Iron-Man-style voice assistant + holographic HUD for Hermes Agent."* FastAPI voice pipeline + WS push-to-talk; **real** Hermes agent behind it; holographic media panels driven by a bundled Hermes plugin (`hud_display`); approval cards; interrupt-aware barge-in; cinematic boot; privacy filter; optional GPU STT sidecar. **This is essentially the browser version of our goal — we reuse its protocol.**
- Others: `Itsme23476/jarvis-hermes-dashboard`, `MultiX0/jarvis` (Windows HUD for a Hermes agent on your server), `kokonuteagle/hermes-jarvis-voice-kit`, `openwarehq/freejarvis`.

### The proven satellite protocol (what our firmware should speak)
Lift `jarvis_ai`'s WebSocket contract verbatim:

```
Client → server:  {"type":"start","sample_rate":16000,"format":"pcm_s16le","channels":1,"conversation":"jarvis-main"}
                  <binary int16 PCM chunks>
                  {"type":"stop"} · {"type":"stop_run"} · {"type":"approval_decision",...}
Server → client:  status · partial_transcript · transcript · run_started{run_id}
                  agent_status{thinking|tool_use|speaking|stopped} · approval_request · error · done{timing}
                  <binary TTS PCM>
```
Brain link = Hermes **Sessions API** `POST /api/sessions/{id}/chat/stream` (SSE) — on Hermes v0.16 the *only* surface that combines **named persistent memory + run ids + tool events + approval events**. (Their lessons-learned list — SSE charset, odd-byte Opus frames, near-silent-audio STT exceptions, FD limits, a plugin tool beats SOUL.md prose for UI actions — is directly reusable.)

### Revised architecture

```
ESP32-P4 satellite (custom fw: WakeNet → mic → Opus)        browser HUD (test harness)
        │  ws://<lxc>:8765/ws   (jarvis_ai protocol)                 │
        └──────────────────────┬──────────────────────────────────────┘
                               ▼
        voice bridge (FastAPI — jarvis_ai server) on assistant host
          STT: faster-whisper (local)   ·   TTS: Piper "British butler" (local)
                               │  Hermes Sessions API (SSE) :8642
                               ▼
                        HERMES AGENT   ──  Ollama (local) / cloud
                   memory · skills · cron · MCP · Home Assistant · media · web · tools
```

### Revised recommendation
1. **Brain = Hermes Agent** on the assistant host (replaces OpenJarvis).
2. **Model = local Ollama** (already understood/deployed) as default provider; add a cloud provider only for frontier-hard tasks. Local STT + local TTS ⇒ **fully private, free**.
3. **Voice = local Piper** butler register (fallback Edge TTS); ElevenLabs optional later.
4. **Our differentiating work = the custom ESP32-P4 satellite firmware** speaking the `jarvis_ai` WS protocol (arc-reactor LVGL UI, on-device WakeNet, Opus/PCM uplink).
5. **Test harness = `jarvis_ai`'s browser HUD** — we can *talk to Jarvis over the LAN today*, before the panel firmware exists.

> Net effect: Hermes gives us L1–L3 capability **off the shelf** (memory, tools, cron proactivity, HA, voice, wake word). Our effort collapses to (a) deploy + configure Hermes locally, (b) the **panel satellite** — the part that is genuinely *ours* and visibly "JARVIS".

---

## 7. Hardware status (what you already have)

Per your build: the panel **already includes battery, camera, speaker, and microphone**, and you have a **power supply and microSD**. The hardware side is essentially **complete** — nothing further to buy for a first working JARVIS.

| Item | Status |
|---|---|
| Panel (ESP32‑P4 + C6, 10.1" touch) | ✅ owned |
| Power supply (5 V ≥ 2 A) | ✅ owned |
| microSD card | ✅ owned |
| Speaker + microphone | ✅ built in (fine for now) |
| Camera (MIPI‑CSI) | ✅ built in — enables vision later |
| Battery (3.7 V Li‑ion) | ✅ built in — gauge on ADC GPIO52 |
| *Optional later:* far‑field mic array | ⬜ only if wake‑word range proves short |

The remaining work is **100% software**: custom firmware + server.

---

## 7b. Optional extras (only if/when you want them)

| Item | Why | Est. cost |
|---|---|---|
| USB‑C data cable (one‑time flash + serial log) | flashing/console | ~$5 |
| *(optional)* Better far‑field mic / mic array | only if the onboard single mic's wake range is too short | $15–40 |
| *(optional)* Enclosure / stand | 242.8 × 158.7 mm cutout | DIY/3D‑print |

**Server:** reuse the home server — confirmed adequate (ample CPU/RAM, Ollama already running). The alternate LXC host at `192.0.2.30` is available as an overflow host.


---

## 8. Software & toolchain to install

**On your workstation/server (build host):**
- **ESP‑IDF v6.x** (ESP‑SR 2.4.7 supports P4 **Rev1 & Rev3** on IDF 6)
- **Git**, `esptool`, `idf.py`, `esp-hosted-mcu` (C6 slave firmware — usually pre‑flashed v0.0.6)
- LVGL 9 + JD9365/GSL3680 drivers (vendor demo or community BSP `profi-max/JC8012P4A1_BSP_ESP32P4`)

**On the server (native Python services — no Docker socket inside the container):**
- `jarvis-server` (custom FastAPI + websockets gateway, see §6.5.4)
- **Ollama — already installed and running** (`127.0.0.1:11434`); reuse `qwen3:8b` / `qwen3.6:35b-a3b`
- `faster-whisper` (ASR) + `piper`/`edge-tts` (TTS) as Python deps / binaries
- An MCP client for tools (Home Assistant, media services, weather, calendar)
- *(Alternative)* deploy via the host's **Apps** UI, or on the **alternate LXC host** `192.0.2.30`

**Useful repos / references**
- Firmware: `github.com/78/xiaozhi-esp32` (ESP32‑P4 supported)
- Server: `github.com/xinnan-tech/xiaozhi-esp32-server`
- Panel BSP: `github.com/profi-max/JC8012P4A1_BSP_ESP32P4` (JD9365 + GSL3680 + ES8311 + C6 + LVGL9/PPA)
- ESPHome community config: HA forum thread *"JC8012P4A1 (Guition ESP32‑P4/ESP32‑C6)"*
- Wake words: `github.com/espressif/esp-sr` (WakeNet) · openWakeWord *hey_jarvis*

---

## 9. Build plan (phased, custom-satellite path)

> The detailed task list is in **§6.5.5**. Summary:

**Phase 0 — Inventory & bench‑test (½ day)**
1. Extract the vendor archive; read schematic + demo.
2. Power the panel; confirm the **factory LVGL demo** runs (display + touch).

**Phase 1 — Toolchain & bring‑up (1–2 days)**
3. Install ESP‑IDF 6; build/flash the vendor or BSP demo. Confirm display, touch, **ES8311 audio in/out**, Wi‑Fi via C6.
4. Update ESP32‑C6 ESP‑Hosted slave firmware if needed (needs ESP‑Prog/UART on `CN5`).

**Phase 2 — Custom firmware (3–7 days)**
5. Scaffold the ESP‑IDF project with the `bsp / audio / wake / ui / net / app` components (§6.5.3); reuse the JC8012P4A1 DSI timings + GSL3680 + ES8311 config.
6. Implement the WebSocket client + JSON control channel; drive a state dot on screen.

**Phase 3 — Custom server (1–2 days)**
7. Build `jarvis-server` (gateway + ASR + LLM + TTS) per §6.5.2/§6.5.4; reuse Ollama; run under systemd.
8. Full voice turn end‑to‑end; tune latency (Opus, VAD, streaming).

**Phase 4 — Integrations (1–3 days)**
9. Wire **MCP tools**: Home Assistant, weather, calendar, media, timers.
10. Personality prompt for "Jarvis".

**Phase 5 — The JARVIS UI (2–5 days)**
11. Custom LVGL theme: idle orb, listening pulse, thinking spinner, speaking waveform; **WS2812 LED** as ambient reactor glow.
12. Bind UI states to the firmware state machine + protocol `state`/`emotion`.

**Phase 6 — Polish (ongoing)**
13. Custom "Jarvis" wake word, camera vision, memory, extra room satellites.

---

## 10. Network / deployment notes (your environment)

- Panel joins SSID on `192.0.2.0/24`; point firmware at the server, e.g. `ws://192.0.2.20:8090/jarvis/v1`.
- Host the assistant server on the **home server** (`192.0.2.20`) — verified: many cores, ample RAM, Ollama already running. The alternate LXC host (`192.0.2.30`) can host overflow services.
- Existing LAN services to integrate: **media server** (`192.0.2.40:8096`), **media-requests** (`192.0.2.40:5055`), gateway `192.0.2.1`.
- Recommend static/DHCP‑reserved IP for the panel. Plain WebSocket is fine; MQTT+UDP is an optional latency upgrade.

---

## 11. Risks & gotchas

- **P4 tooling is bleeding‑edge.** Arduino‑esp32 P4 is beta; ESPHome P4/MIPI‑DSI support is new/buggy. **Plan on ESP‑IDF 6 + LVGL.** (Vendor archive may ship older IDF versions — expect to upgrade.)
- **No native Wi‑Fi** on P4 — everything network goes through the C6 (`ESP‑Hosted`). If Wi‑Fi misbehaves, the C6 slave firmware version is the usual culprit.
- **P4 silicon revisions** (Rev1 vs Rev3) changed power rails; early/late boards differ. Confirm your revision before copying DSI/PSRAM settings.
- **Spec variance:** official sheet says 32 MB PSRAM / 16 MB flash; reseller listings sometimes say 8 MB PSRAM. Verify before sizing assets.
- **Single onboard mic** → wake word range is limited; add a mic array for a real room assistant.
- **Custom "Jarvis" wake word** is not free/instant with ESP‑SR (Espressif customization). Budget for it or pick another trigger phrase.
- **Latency:** with a modern multi-core CPU and ample RAM and Ollama local, the whole pipeline runs on CPU. Still, use a small fast model (`qwen3:8b`/`qwen3-fast`) for the live conversation loop and reserve the 35B for hard queries — and stream tokens to the screen so it *feels* instant.
- **Container durability:** if the dev container can be recreated, keep
  server code + models on a durable dataset (e.g. a repo checkout or a ZFS
  dataset) and add a systemd unit so the assistant auto-starts.
- **DSI timings:** generic JD9365 configs fail — use the known‑good JC8012P4A1 timings (60 MHz pclk, 1 Gbps/lane, hsync back20/pulse20/front40, vsync back8/pulse4/front20).

---

## 12. Rough cost summary

| Bucket | Low | High |
|---|---|---|
| Panel (owned) | — | — |
| Power supply + microSD + speaker + cable | ~$25 | ~$45 |
| Optional: battery, camera, mic array, enclosure | ~$35 | ~$90 |
| **Server** (reuse the home server) | $0 | $0 |
| Cloud LLM/TTS at light home use | $0–5/mo | ~$20/mo |
| Custom wake word (one‑off, if wanted) | varies | varies |

---

## 13. Open decisions before build

**Resolved:**
- ✅ **Brain = Hermes Agent** (not OpenJarvis) — see §6.8.
- ✅ **Host = dedicated assistant host** at `192.0.2.10` (Debian 13).
- ✅ **Cloud vs local** — the brain stays **local** (Ollama + local STT/TTS); cloud is optional, not required.

**Still open:**
1. **Wake word:** stock ESP-SR word now, or commission a custom "Jarvis"? (On-device WakeNet; Hermes also has its own wake word for browser/desktop — ours is for the panel.)
2. **TTS voice:** local **Piper** butler (default) vs **Edge TTS** (more natural, still free) vs ElevenLabs (cloud, best) vs voice clone.
3. **LLM provider:** **local Ollama** only (private, free) vs local + a cloud provider for hard tasks.
4. **Reuse `jarvis_ai` server/HUD** as the bridge + test harness, or write our own thin bridge from its protocol?
5. **UI ambition:** functional state UI vs full animated arc-reactor theme.
6. **Tools scope for v1:** Home Assistant only, or also media services + weather/calendar?
7. **Camera/vision** in scope now or later?

---

## 14. Voice bridge deployed (assistant host) — build status

**Resolved open decision #4:** reuse `eadmin2/jarvis_ai` as the bridge (satellite protocol + HUD), adapted:
- `server/server.py` — STT swapped from `RealtimeSTT` to direct **faster-whisper** (`small.en`, CPU int8); TTS swapped from ElevenLabs to **Piper** (`en_GB-alan-medium`, resampled 22050→16000).
- Hermes brain via **Sessions API** `POST /api/sessions/{id}/chat/stream` (SSE) — persistent memory, stoppable runs, tool/approval events.
- SOUL.md rewritten as the dry-witted British-butler persona (voice-first output rules).
- `llmproxy.py` (loopback 11440→11434) forces `reasoning_effort: none` + `think: false` on every Ollama request — Hermes does **not** send reasoning controls to local-Ollama custom profiles, so gpt-oss otherwise "thinks" (200+ hidden tokens/turn).

**Services on assistant host (all systemd, auto-start):**
| Service | Port | Notes |
|---|---|---|
| `ollama` | 127.0.0.1:11434 | gpt-oss:20b, 64K ctx, keep-alive 30m |
| `llmproxy` | 127.0.0.1:11440 | injects reasoning-off; logs to `/var/log/llmproxy.log` |
| `hermes-gateway` | 127.0.0.1:8642 | Hermes API server (Bearer `API_SERVER_KEY`) |
| `jarvis-voice` | 0.0.0.0:8765 | satellite WS `/ws` + HUD |

**Latency work (68 s → ~5 s steady state):**
- Default `agent.reasoning_effort` was `medium`; the voice turn ran full "thinking" (200+ tokens at ~8 tps). Global `agent.reasoning_effort: none` + proxy injection killed it.
- **Title generation was the hidden 30 s tax**: `auxiliary.title_generation` fired per session, timed out at 30 s in the single Ollama slot, and serialized the main turn. Disabled (`enabled: false`, `model_upgrade_enabled: false`). Also disabled `kanban.review_dispatch`.
- Steady-state per turn: system prompt (**9,733 tokens**) prefix-caches in Ollama → prefill ≈ 0.7 s; generation 27–69 tokens @ ~8 tps ≈ 3–8 s. First turn after idle pays one ~23 s tail prefill.
- Agentic tool loops (e.g. "dim the lights" → `search_files` + `web_search`) still run multi-round-trip and take ~60–120 s on CPU — inherent; fix later via fast-model routing or a real Home Assistant toolset.

**Open decisions updated:** #4 **resolved** (reuse jarvis_ai). #5 (UI ambition) now the HUD exists as-is; satellite panel UI is custom (arc-reactor, own phase). #6 (tools scope) — no HA instance yet; agent currently answers honestly when tools are absent. #3 partially: local-only default, cloud can be added later for hard tasks.

---

*Sources: Guition official spec sheet (JC8012P4A1C_I_W_Y, EN v1.1); Guition product page; profi‑max JC8012P4A1 BSP (pinout); ESPHome HA community thread; atomic14 ESP32 board DB; 78/xiaozhi‑esp32; xinnan‑tech/xiaozhi‑esp32‑server docs; Espressif ESP‑SR WakeNet docs; openWakeWord; NousResearch/hermes-agent (README + docs: voice-mode, providers, api-server, homeassistant, wake-word, personality, memory, cron); eadmin2/jarvis_ai (README + docs/ARCHITECTURE.md + docs/SETUP.md + observed behavior).*
