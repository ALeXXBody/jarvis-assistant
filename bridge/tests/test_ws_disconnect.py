#!/usr/bin/env python3
"""Integration check: /ws must close cleanly when the client disconnects.

Reproduces the exact failure that produced 16 unhandled ASGI tracebacks in the
service log. Starlette's receive() *returns* {"type": "websocket.disconnect"}
and raises WebSocketDisconnected (a plain RuntimeError, and NOT a
WebSocketDisconnect subclass) only on the *next* call. The endpoint caught only
WebSocketDisconnect, so the second call escaped.

Drives the real endpoint with a real starlette WebSocket fed by a scripted
receive(). No network, no httpx, no models loaded.

This is the one test in this directory that needs the service's own venv
(starlette + fastapi), so it runs on the assistant host:

    /opt/jarvis/venv/bin/python3 test_ws_disconnect.py

Run from a bare python3 without those packages it prints SKIP and exits 0.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import sys
from pathlib import Path

try:
    from starlette.websockets import WebSocket, WebSocketDisconnected
except ImportError:
    print(
        "SKIP: needs starlette + fastapi (run with the service venv on the "
        "assistant host: /opt/jarvis/venv/bin/python3)"
    )
    raise SystemExit(0)

CANDIDATES = [
    Path(__file__).resolve().parents[1] / "server.py",
    Path("/opt/jarvis/jarvis_ai/server/server.py"),
]
SERVER = next((p for p in CANDIDATES if p.exists()), None)
if SERVER is None:
    sys.exit(f"server.py not found in: {[str(p) for p in CANDIDATES]}")


def load_server():
    spec = importlib.util.spec_from_file_location("jarvis_server_under_test", SERVER)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


mod = load_server()
print(f"loaded {SERVER}")
_gone = getattr(mod, "CLIENT_GONE", None)
print("CLIENT_GONE =", [c.__name__ for c in _gone] if _gone else "ABSENT (server.py looks unfixed)")


def make_ws(script: list):
    """A real starlette WebSocket whose receive() walks a fixed script.

    Calling receive() more times than the script allows is an AssertionError,
    which is what makes this a regression test: without the early `break`, the
    endpoint spins past the close frame and calls receive() a third time.
    """
    scope = {
        "type": "websocket", "path": "/ws", "raw_path": b"/ws", "headers": [],
        "query_string": b"", "client": ("127.0.0.1", 5555),
        "server": ("127.0.0.1", 8765), "scheme": "ws", "root_path": "",
        "subprotocols": [], "state": {},
    }
    sent: list = []
    queue = iter(script)

    async def receive():
        # Yield like real socket I/O does, so tasks created by the endpoint
        # (e.g. the turn task) actually get scheduled before the next message.
        await asyncio.sleep(0)
        try:
            item = next(queue)
        except StopIteration:
            raise AssertionError(
                "receive() called more times than the script provides — the "
                "endpoint did not stop at the disconnect frame"
            ) from None
        if isinstance(item, BaseException):
            raise item
        return item

    async def send(message):
        sent.append(message)

    return WebSocket(scope, receive, send), sent


async def test_premise():
    """The bug in one line: the two exceptions are unrelated."""
    assert not issubclass(WebSocketDisconnected, mod.WebSocketDisconnect), (
        "premise changed: if it were a subclass the original code would have worked"
    )
    assert issubclass(WebSocketDisconnected, RuntimeError)
    gone = getattr(mod, "CLIENT_GONE", None)
    assert gone is not None, "CLIENT_GONE missing — server.py looks unfixed"
    assert WebSocketDisconnected in gone
    assert mod.WebSocketDisconnect in gone
    print("PASS WebSocketDisconnected is a RuntimeError, not a WebSocketDisconnect")


async def test_clean_disconnect():
    """A close frame, then no further receive() — must return, not raise."""
    ws, sent = make_ws([
        {"type": "websocket.connect"},
        {"type": "websocket.disconnect", "code": 1000},
    ])
    await mod.websocket_endpoint(ws)
    assert sent[0]["type"] == "websocket.accept", sent[0]
    assert any(m["type"] == "websocket.send" for m in sent), "no status message sent"
    assert ws not in mod.WS_CLIENTS, "socket leaked in WS_CLIENTS"
    print("PASS close frame returns cleanly (no unhandled exception)")


async def test_already_disconnected():
    """If receive() raises WebSocketDisconnected it must still be caught."""
    ws, _ = make_ws([
        {"type": "websocket.connect"},
        WebSocketDisconnected(
            'Cannot call "receive" once a disconnect message has been received.'
        ),
    ])
    await mod.websocket_endpoint(ws)
    assert ws not in mod.WS_CLIENTS, "socket leaked in WS_CLIENTS"
    print("PASS WebSocketDisconnected is caught")


async def test_turn_cancelled_on_disconnect():
    """A turn in flight must be cancelled when the client goes away.

    _run_turn is swapped for a stub so no model is loaded and no Hermes call is
    made; the endpoint reaches it through the module global.
    """
    started = asyncio.Event()
    cancelled = asyncio.Event()

    async def fake_run_turn(ws, pipeline, conn):
        started.set()
        try:
            await asyncio.sleep(30)
        except asyncio.CancelledError:
            cancelled.set()
            raise

    original = mod._run_turn
    mod._run_turn = fake_run_turn
    try:
        ws, _ = make_ws([
            {"type": "websocket.connect"},
            {"type": "websocket.receive",
             "text": json.dumps({"type": "start", "sample_rate": 16000,
                                 "format": "pcm_s16le", "channels": 1})},
            {"type": "websocket.receive", "text": json.dumps({"type": "stop"})},
            {"type": "websocket.disconnect", "code": 1000},
        ])
        await mod.websocket_endpoint(ws)
        assert started.is_set(), "the stub turn never started"
        await asyncio.wait_for(cancelled.wait(), timeout=1.0)
        assert ws not in mod.WS_CLIENTS, "socket leaked in WS_CLIENTS"
    finally:
        mod._run_turn = original
    print("PASS in-flight turn cancelled when the client disconnects")


async def main() -> int:
    failures = []
    for test in (test_premise, test_clean_disconnect, test_already_disconnected,
                 test_turn_cancelled_on_disconnect):
        try:
            await test()
        except Exception as exc:
            failures.append(f"{test.__name__}: {type(exc).__name__}: {exc}")
            print(f"FAIL {test.__name__}: {type(exc).__name__}: {exc}")
    if failures:
        print(f"\n{len(failures)} of 4 checks failed")
        return 1
    print("all ws-disconnect tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
