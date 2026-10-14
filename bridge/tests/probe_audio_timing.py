#!/usr/bin/env python3
"""Measure when sound actually reaches the client — the only latency that matters.

Streams a WAV through the real /ws endpoint and timestamps every incoming binary
frame, grouping them into "bursts" (a gap > 0.4 s starts a new one). With the
spoken ack off there is one burst, seconds after end-of-speech. With it on there
are two: the short cue, then the reply.

Needs `websockets`, so run it with the service venv on the assistant host:

    /opt/jarvis/venv/bin/python3 bridge/tests/probe_audio_timing.py /tmp/piper-test.wav

This is a diagnostic, not a test: it asserts nothing, it prints numbers.
"""

import asyncio
import json
import sys
import time
import wave

import websockets

WAV = sys.argv[1]
CONV = sys.argv[2] if len(sys.argv) > 2 else "jarvis-debug"
URL = "ws://127.0.0.1:8765/ws"
GAP = 0.4  # a silence longer than this separates two bursts


async def main() -> None:
    with wave.open(WAV, "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1, "need 16k mono"
        pcm = w.readframes(w.getnframes())

    async with websockets.connect(URL, max_size=None) as ws:
        await ws.send(json.dumps({
            "type": "start", "sample_rate": 16000, "format": "pcm_s16le",
            "channels": 1, "conversation": CONV,
        }))
        step = 16000 * 2 * 0.08  # 80 ms of s16le, paced like the panel
        for i in range(0, len(pcm), int(step)):
            await ws.send(pcm[i:i + int(step)])
            await asyncio.sleep(0.08)

        t_stop = time.perf_counter()
        await ws.send(json.dumps({"type": "stop"}))
        print("sent stop (end of speech)")

        frames: list[tuple[float, int]] = []
        events: list[tuple[float, str]] = []
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=180)
            now = time.perf_counter() - t_stop
            if isinstance(msg, bytes):
                frames.append((now, len(msg)))
                continue
            ev = json.loads(msg)
            kind = ev.get("type")
            if kind in ("transcript", "partial_transcript"):
                events.append((now, f"{kind}: {ev.get('text')!r}"))
            elif kind == "agent_status":
                events.append((now, f"agent_status: {ev.get('state')}"))
            elif kind == "error":
                events.append((now, f"ERROR: {ev.get('message')}"))
                break
            elif kind == "done":
                events.append((now, "done"))
                break

    bursts: list[list[float]] = []
    for t, n in frames:
        if bursts and t - bursts[-1][1] < GAP:
            bursts[-1][1] = t
            bursts[-1][2] += n
        else:
            bursts.append([t, t, n])

    print(f"\n--- audio bursts (a gap > {GAP}s starts a new one) ---")
    if not bursts:
        print("  NO AUDIO")
    for i, (a, b, n) in enumerate(bursts, 1):
        print(f"  burst {i}: first byte {a:6.2f}s after end-of-speech, "
              f"span {b - a:5.2f}s, {n:6d} bytes")
    print("\n--- events ---")
    for t, label in events:
        print(f"  {t:6.2f}s  {label}")


if __name__ == "__main__":
    asyncio.run(main())
