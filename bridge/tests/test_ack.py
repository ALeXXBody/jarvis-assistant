#!/usr/bin/env python3
"""Drive the spoken-ack task against fake sockets, with no piper/network.

server.py's real _start_ack / _speak_ack / _cancel_pending_ack and ConnState
are lifted out with ast (the module needs faster_whisper etc. to import) and
run under asyncio against stubs. The scenarios covered are:

  1. a fast reply cancels the ack before it makes a sound;
  2. a slow reply speaks it once, and does not pollute the turn's timings;
  3. ack_after_seconds <= 0 schedules nothing;
  4. a reply arriving mid-ack waits on tts_lock rather than interleaving two
     PCM streams on the same socket;
  5. successive turns rotate through ack_texts.
"""

from __future__ import annotations

import ast
import asyncio
import pathlib
import sys
import time

SERVER_PY = pathlib.Path(__file__).resolve().parents[1] / "server.py"


def _def_source(src: str, node: ast.AST) -> str:
    """Source for a def/class including any decorator lines above it."""
    decos = getattr(node, "decorator_list", None) or []
    if not decos:
        return ast.get_source_segment(src, node)
    first = min(d.lineno for d in decos)
    lines = src.splitlines()
    # Linenos are 1-based; include the decorator lines through the end of the def.
    end = node.end_lineno
    return "\n".join(lines[first - 1:end])


def build_ns():
    src = SERVER_PY.read_text(encoding="utf-8")
    tree = ast.parse(src)

    methods = {"_start_ack", "_speak_ack", "_cancel_pending_ack"}
    pieces: list[str] = ["from dataclasses import dataclass, field", "import asyncio"]

    conn_src = None
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == "ConnState":
            conn_src = _def_source(src, node)
        if isinstance(node, ast.ClassDef) and node.name == "TurnTiming":
            pieces.append(_def_source(src, node))
        if isinstance(node, ast.ClassDef) and node.name == "VoicePipelineServer":
            for sub in node.body:
                if isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef)) and sub.name in methods:
                    body = _def_source(src, sub)
                    # Re-indent the method body back to module level.
                    pieces.append("\n".join(
                        line[4:] if line.startswith("    ") else line
                        for line in body.splitlines()
                    ))

    assert conn_src, "ConnState not found"
    pieces.append(conn_src)

    ns: dict = {}
    exec("\n\n".join(pieces), ns)  # noqa: S102
    return ns


ns = build_ns()
ConnState = ns["ConnState"]
TurnTiming = ns["TurnTiming"]
_cancel_pending_ack = ns["_cancel_pending_ack"]
_start_ack = ns["_start_ack"]
_speak_ack = ns["_speak_ack"]


class FakeWS:
    """Records the order of text events and audio writes."""

    def __init__(self):
        self.events: list[tuple] = []
        self.sent_bytes = 0

    async def send_json(self, obj):
        self.events.append(("json", obj.get("type"), obj.get("state")))

    async def send_bytes(self, data: bytes):
        self.events.append(("bytes", len(data)))
        self.sent_bytes += len(data)


class FakePipeline:
    """Stands in for VoicePipelineServer: stub config + stub TTS."""

    def __init__(self, cfg, synth_delay=0.0):
        self.cfg = cfg
        self.synth_delay = synth_delay
        self.synth_starts: list[str] = []

    def _start_ack(self, ws, timing, conn):
        return _start_ack(self, ws, timing, conn)

    async def _speak_ack(self, ws, timing, conn, delay, texts):
        return await _speak_ack(self, ws, timing, conn, delay, texts)

    async def _send_tts_sentence(self, ws, sentence, timing):
        self.synth_starts.append(sentence)
        # Model piper: a pause before the first byte, then streamed chunks.
        await asyncio.sleep(self.synth_delay)
        for _ in range(3):
            await ws.send_bytes(b"\x00\x01" * 160)
            await asyncio.sleep(0.01)


CFG = {"hermes": {"ack_after_seconds": 0.05, "ack_texts": ["On it.", "Still working."]}}


async def scenario_fast_reply():
    """The reply beats the timer: the ack must never produce audio."""
    pipe = FakePipeline(CFG)
    ws, conn = FakeWS(), ConnState()
    timing = TurnTiming(turn_id=1)

    task = pipe._start_ack(ws, timing, conn)
    assert task is not None, "ack task should be scheduled"
    await asyncio.sleep(0.01)          # reply starts before the 50ms timer
    _cancel_pending_ack(task, conn)
    try:
        await task
    except asyncio.CancelledError:
        pass
    assert ws.sent_bytes == 0, f"ack spoke on a fast reply: {ws.events}"
    assert pipe.synth_starts == [], f"ack synthesized: {pipe.synth_starts}"


async def scenario_slow_reply():
    """Nothing else happens: the ack speaks once, then stops."""
    pipe = FakePipeline(CFG)
    ws, conn = FakeWS(), ConnState()
    timing = TurnTiming(turn_id=2)

    task = pipe._start_ack(ws, timing, conn)
    await asyncio.wait_for(asyncio.shield(task), timeout=1.0)
    assert pipe.synth_starts == ["On it."], pipe.synth_starts
    assert ws.sent_bytes > 0, "ack produced no audio"
    assert conn.ack_started is False, "ack_started not cleared"
    assert conn.ack_index == 1, conn.ack_index
    # The cue must not pollute the turn's latency numbers.
    assert timing.first_tts_audio_byte_monotonic is None


async def scenario_no_ack_when_disabled():
    pipe = FakePipeline({"hermes": {"ack_after_seconds": 0, "ack_texts": ["On it."]}})
    ws, conn = FakeWS(), ConnState()
    task = pipe._start_ack(ws, TurnTiming(turn_id=3), conn)
    assert task is None, "ack scheduled with ack_after_seconds=0"


async def scenario_interleave_guard():
    """A reply arriving mid-ack must wait, not write over the cue's audio."""
    pipe = FakePipeline(CFG, synth_delay=0.0)
    ws, conn = FakeWS(), ConnState()
    timing = TurnTiming(turn_id=4)

    ack_task = pipe._start_ack(ws, timing, conn)
    await asyncio.sleep(0.06)                    # let the ack begin speaking
    assert conn.ack_started, "ack did not start"

    order: list[str] = []

    async def reply():
        async with conn.tts_lock:
            order.append("reply")

    reply_task = asyncio.create_task(reply())
    await asyncio.sleep(0.001)
    assert order == [], "reply wrote while the ack held the lock"
    await asyncio.wait_for(asyncio.gather(ack_task, reply_task), timeout=1.0)
    assert order == ["reply"], order
    assert conn.ack_started is False


async def scenario_ack_rotates():
    pipe = FakePipeline(CFG)
    ws, conn = FakeWS(), ConnState()
    for i in range(3):
        task = pipe._start_ack(ws, TurnTiming(turn_id=10 + i), conn)
        await asyncio.wait_for(asyncio.shield(task), timeout=1.0)
    assert pipe.synth_starts == ["On it.", "Still working.", "On it."], pipe.synth_starts


async def main():
    started = time.perf_counter()
    await scenario_fast_reply()
    await scenario_slow_reply()
    await scenario_no_ack_when_disabled()
    await scenario_interleave_guard()
    await scenario_ack_rotates()
    print(f"all ack tests passed in {time.perf_counter() - started:.2f}s")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except AssertionError as exc:
        print(f"FAILED: {exc}")
        sys.exit(1)
