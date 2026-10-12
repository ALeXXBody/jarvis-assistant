#!/usr/bin/env python3
"""Loopback OpenAI-compat proxy: Hermes -> this (127.0.0.1:11440) -> Ollama (127.0.0.1:11434).

Why: Hermes does not send reasoning controls to local-Ollama custom profiles, so gpt-oss
"thinks" (0.5-2k hidden tokens) on every turn even when agent.reasoning_effort is "none".
This proxy forces `reasoning_effort: none` + `think: false` onto every chat request so the
VOICE chain stays fast, and logs per-request latency + token counts.

It is JSON-transparent for non-chat endpoints and streams chat completions back verbatim.
"""

import json
import re
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = "http://127.0.0.1:11434"
LOG = open("/var/log/llmproxy.log", "a", buffering=1)


def log(msg: str) -> None:
    LOG.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")


def mutate_body(raw: bytes, path: str) -> tuple[bytes, bool]:
    """Add reasoning-off controls to chat bodies. Returns (new_body, mutated)."""
    if raw is None or not raw.strip():
        return raw, False
    try:
        obj = json.loads(raw) if raw else {}
    except Exception:
        return raw, False
    if not isinstance(obj, dict):
        return raw, False

    mutated = False
    if "/v1/chat/completions" in path:
        # OpenAI-compat knobs Ollama honors (verified on 0.40.x)
        for key, val in (("reasoning_effort", "none"), ("think", False)):
            if obj.get(key) is None:
                obj[key] = val
                mutated = True
    elif path.endswith("/api/chat"):
        if obj.get("think") is None:
            obj["think"] = False
            mutated = True
        opts = obj.setdefault("options", {})
        if opts.get("reasoning_effort") is None:
            opts["reasoning_effort"] = "none"
            mutated = True
    elif path.endswith("/api/generate"):
        if obj.get("think") is None:
            obj["think"] = False
            mutated = True
    if not mutated:
        return raw, False
    return json.dumps(obj).encode(), True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        self._forward(b"", read_body=False)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length else b""
        self._forward(body, read_body=True)

    def do_HEAD(self):
        self._forward(b"", read_body=False)

    def _forward(self, body: bytes, read_body: bool) -> None:
        t0 = time.perf_counter()
        url = UPSTREAM + self.path
        headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
        new_body, mutated = (b"", False)
        if read_body:
            new_body, mutated = mutate_body(body, self.path)

        req = urllib.request.Request(url, data=new_body if read_body else None,
                                     headers=headers, method=self.command)
        req.add_header("Connection", "keep-alive")
        try:
            resp = urllib.request.urlopen(req, timeout=900)
        except urllib.error.HTTPError as e:
            resp = e
        except Exception as e:
            log(f"{self.command} {self.path} UPSTREAM-ERROR {e}")
            self.send_response(502)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        dt = round((time.perf_counter() - t0) * 1000)
        status = resp.getcode()
        ctype = resp.headers.get("Content-Type", "")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        if status == 200:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            # error responses are small; inline them
            data = resp.read()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            log(f"{self.command} {self.path} status={status} body_mut={mutated} ms={dt}")
            resp.close()
            return
        self.end_headers()

        total = 0
        try:
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                total += len(chunk)
                self.wfile.write(f"{len(chunk):X}\r\n".encode())
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except Exception as e:
            log(f"{self.command} {self.path} STREAM-ERR {e}")
        finally:
            resp.close()
        dt = round((time.perf_counter() - t0) * 1000)
        log(f"{self.command} {self.path} status={status} body_mut={mutated} bytes={total} ms={dt}")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 11440
    log(f"llmproxy listening on 127.0.0.1:{port} -> {UPSTREAM} (reasoning forced off)")
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.serve_forever()