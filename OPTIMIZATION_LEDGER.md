# Optimization ledger — making the conversation feel human

Working record of the fluency program. Every change lands here: the plan,
sub-plans, current status, and the measured numbers. Where a decision is
"locked", it was taken with evidence and should not be relitigated without a
new measurement saying otherwise.

## Goal

A turn that *feels* like talking to a person: the assistant starts answering
almost immediately, acknowledges you while it works, lets you cut it off, and
never leaves you staring at THINKING for a minute.

## Current baseline (measured)

| phase | number | source |
|---|---|---|
| end-of-speech → first audio (warm everything, ack off) | 14.8 s | live probe, Oct 14 |
| end-of-speech → first audio (Piper warmed, ack at 2.0 s, delay fixed) | **2.9 s** | live probe, Oct 14 |
| STT finalize (whisper `small.en`, brain CPU) | 1.1–2.9 s | TURN TIMING journal |
| LLM time-to-first-token (warm) | 5.9 s | TURN TIMING turn 15 |
| LLM time-to-first-token (after >30 min idle) | **115.7 s** | TURN TIMING turn 14 — FIXED by keeping the model resident |
| Piper first audio after reply text starts | 0.04–0.13 s | TURN TIMING/turn 15 |
| panel mouth-open → panel hears "speaking" | ~0 s | bridge→ws |
| panel µC RMS gate: typical silence-hang before EOS | 1.0–3.7 s | panel logs (`capture stop (silence, …)`) |

## Decisions locked

1. **LLM stays `gpt-oss:20b`, resident forever** — `OLLAMA_KEEP_ALIVE=-1`
   (16 GB in a 1 TB box). Cold first turns after a *reboot* only; bridge
   warm-up will cover that too.
2. **Spoken ack at 2.0 s stays**, with dynamic cancellation: as the pipeline
   gets faster, fast turns will never hear it (the ack self-cancels when real
   audio wins the race).
3. **esp_hosted stays on the 3.x line** (2.x killed the radio — documented
   in PANEL_AUDIT). "Newer" does not mean "2.x".
4. **OTA protocol: no version file**, descriptor-vs-descriptor (v0.1.13).
5. **Build discipline**: both envs rebuilt green before any release; no
   blind "should-work" pushes.

## Sub-plans

### P1 — brain streaming (no panel changes needed) — *status: in progress*

| # | item | why | status |
|---|---|---|---|
| A | Ollama keep-alive `-1`, measure warm TTFT | kills the 116 s cold start | **done** (cold 24 s, warm 5.2 s) |
| B | warm the LLM during bridge startup | covers the one cold turn after a brain reboot | **done** (`LLM warmed (7.4s)` in the journal at start) |
| C | faster first clause: first chunk starts small (~24 chars), steady-state chunks 60+ | shaves 0.3–1 s off "first words" | **done** (`voice.first_chunk_chars: 24`) |
| D | STT: finalise faster — vad_filter + use the last partial as the final transcript when no better final arrives in time | 1–2 s saved per turn; fixes "No transcript detected" tails | open |
| E | measure the full warm chain through the real bridge (my `ws_e2e_test.py` harness) and put today's numbers in this table before/after every change | no blind tuning | **done** |

### P1.5 — tool discipline on the voice channel (the "hello costs 2 minutes" killer)

Full CLI toolset stays enabled **by explicit user decision**. What changed is
the agent's *prompt*: `hermes.instructions` now carries a tool-discipline
clause — tools only when the request genuinely needs house/system state or an
action; greetings, small talk, mental arithmetic and general knowledge answer
directly. Measured on the same test utterance ("What is 17 × 4?"):

| | before | after |
|---|---|---|
| tools run | `execute_code` + `terminal` | 0 |
| LLM TTFT | 25.8 s | 6.6 s |
| end-of-speech → first audio | 28.8 s | **8.0 s** |
| STT finalize | 2.9 s | 1.35 s |

(The 116–133 s spikes from Oct 14 were the same class: tool runs + model
eviction; both are handled — eviction by keep-alive, tool grabbing by the
discipline clause.)

### P2 — panel audio front-end (ESP-SR build, needs one USB flash)

The biggest *human-feel* win: real VAD endpointing (~300–500 ms silence =
end of speech, instead of the 1–4 s RMS-hang), AEC, and **voice barge-in**
(cut the assistant off mid-sentence — currently only possible by tapping).

The SR build is compiled (both envs green, `srmodels.bin` packed) but not
yet flashed to the panel; its partition table differs from the panel's, so
the first hop must be the browser installer's *fresh install* path
(speeds-up OTA later; OTA-ing into a SR table is already safe-by-fallback
but won't enable the AFE).

| # | item | status |
|---|---|---|
| F | flash SR build (fresh install, v0.1.x-sr) on the panel | waiting on the user's flash session |
| H2 | gate tightening (no-speech abort at 3 s instead of 20 s of streamed silence; post-speech hang 900 -> 700 ms) | **committed** — builds green both envs, ships with the next flashing session |
| H3 | keyboard taps dropped — the single global 350 ms debounce ate every keystroke during fast typing ("press 0 two-three times") | **committed** — keyboard re-taps at 90 ms (JARVIS_KBD_KEY_MS), toggle zones stay at 350 ms, 60 ms contact-bounce floor at the touch layer; builds green both envs |
| G | tune `JARVIS_AFE_VAD_SILENCE_MS` / barge-in arm times from live logs | blocked on F |
| H | lower the RMS-silence gate/timeout for the *legacy* build too (safe change) | open |

### P3 — library / tool version audit (registry-checked Oct 15 — user-specified requirement)

Policy reminder: user asked that the panel always run the *latest* component
drivers and tools. The full dependency stack was therefore checked against
the ESP Component Registry and the tooling upstream. Result: **everything
shipped is already the newest released version** — with two upgrade trains
not yet available and one piece of direct evidence that here "newer" does
not mean "better".

| component | shipped (locked) | registry latest | verdict |
|---|---|---|---|
| esp_hosted | 3.0.9 | **3.0.9** | newest (2.x is the radio killer — see P-panel history) |
| esp_wifi_remote | 1.6.5 | **1.6.5** | newest; pinned pair with hosted 3.0.9 |
| wifi_remote_over_eppp | 0.3.3 | **0.3.3** | newest |
| eppp_link | 1.1.6 | **1.1.6** | newest |
| esp_serial_slave_link | 1.1.2 | **1.1.2** | newest |
| esp_websocket_client | 1.8.0 | **1.8.0** | newest |
| es8311 | 1.0.0~1 | **1.0.0** | newest |
| esp_codec_dev | 1.6.2 | 2.0.0-**beta5** | **hold** — 2.0 is pre-release; move when 2.0 stable lands |
| esp-sr (SR env) | 2.6.0 | **2.6.0** | newest |
| ESP-IDF / pioarduino | 5.5.5 (pioarduino develop, newest release 55.03.312-1 is also 5.5.5-based) | IDF 5.6.x exists upstream-only | **hold** — pioarduino's release train has not shipped 5.6; recheck per release |
| PlatformIO core | 6.2.0 | same | current |
| esptool | 5.5.0 (via toolchain) | same | current |
| JD9365 LCD driver | **vendored** (community BSP for this exact board) | registry "newer" version | **proves newer ≠ better**: the registry build boots this panel black (documented in PANEL_AUDIT); the vendored one works |
| GSL3680 touch driver | hand-written (no public driver exists) | none | keep |

Standing rule going forward: on every release train (each `pio run` round or
before a release), a 60-second registry diff is enough to see if any of the
"newest" verdicts above went stale; upgrades land immediately when the
registry moves, gated by the build + one verified flash as usual.

### P4 — panel UX polish (after P2)

Interrupt-detection misfires (the "speaking → idle 187 ms" flips), status
line accuracy ("thinking" showing during the ack while audio already plays),
audio DMA streaming (continuous I2S feed instead of burst chunks) — all
parked until the AFE is on-device so we tune against the real thing.

## Execution order

A → E → C → tool-discipline (P1.5) → D (STT vad + graceful no-speech)
→ panel gate tightening (committed, ships with the next flash)
→ (user flashes SR build + latest stack) → G → H → remaining P4 items.
