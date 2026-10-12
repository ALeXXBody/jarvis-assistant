#!/usr/bin/env python3
"""
JARVIS panel log collector — the server half of the browser-based debug loop.

The panel is plugged into the *user's* computer. Their browser (console.html)
opens it with Web Serial and streams the log lines here with POST /lines.
This process keeps a ring buffer and serves it:

  GET  /          live viewer page (dark terminal, SSE feed)
  GET  /logs      Server-Sent-Events stream
  GET  /logs.txt  the whole buffered log as plain text (curl-friendly)
  GET  /health    {"lines": n, "clients": n, "since": ts}
  POST /lines     body: text/plain, one or more lines — append to the buffer

  CORS is fully open (the collector is fed from a file:// page or any origin),
  and nothing is written to disk.

Run:  python3 log-server.py --port 8766
"""
import json
import queue
import sys
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Lock
from urllib.parse import urlparse

PORT_NUM = 8766
MAX_LINES = 4000

_buffer = deque(maxlen=MAX_LINES)
_lock = Lock()
_subs = []                       # per-subscriber queue.Queue (SSE clients)
_started = time.time()


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>J.A.R.V.I.S. panel — collected console</title>
<style>
 :root{color-scheme:dark}
 *{box-sizing:border-box}
 body{margin:0;height:100vh;display:flex;flex-direction:column;font:14px/1.5 ui-monospace,Menlo,Consolas,monospace;background:#0b0f1a;color:#c9d4f2}
 header{display:flex;align-items:center;gap:.6rem;padding:.55rem .9rem;background:#101a33;border-bottom:1px solid #23335e;flex-wrap:wrap}
 header b{color:#7fd4ff;font-family:system-ui,sans-serif}
 button,input[type=text]{background:#141f3d;color:#dbe4ff;border:1px solid #2b3c6e;border-radius:6px;padding:.35rem .7rem;font:inherit}
 button:hover{background:#1b2a52;cursor:pointer}
 #filter{flex:1;min-width:12rem}
 #status{color:#7a89b8;margin-left:auto}
 main{flex:1;overflow-y:auto;padding:.5rem .9rem;white-space:pre-wrap;word-break:break-all}
 .er{color:#ff7a90}.wr{color:#ffd479}.in{color:#c9d4f2}.db{color:#6f7fa8}.tx{color:#b7a6ff}
</style></head>
<body>
<header><b>J.A.R.V.I.S. panel — collected console</b>
<button id="clear">Clear view</button><button id="save">Save</button><button id="copy">Copy</button>
<input id="filter" type="text" placeholder="filter lines (substring)"><span id="status">connecting…</span></header>
<main id="log"></main>
<script>
"use strict";
const log=document.getElementById("log"),status=document.getElementById("status"),
      filterEl=document.getElementById("filter");
let lines=[],stick=true;const MAX=20000;
fetch("/health").then(r=>r.json()).then(h=>status.textContent=h.lines+" lines buffered");
fetch("/logs.txt").then(r=>r.text()).then(t=>t.split("\n").forEach(append));
const es=new EventSource("/logs");
es.onopen=()=>status.textContent="live (SSE)";
es.onerror=()=>status.textContent="reconnecting…";
es.onmessage=ev=>append(ev.data);
function levelOf(l){if(/\bE ?\(\d+\)/.test(l))return"er";if(/\bW ?\(\d+\)/.test(l))return"wr";
 if(/\bD ?\(\d+\)/.test(l))return"db";if(/^>>>/.test(l))return"tx";return"in"}
function append(l){if(!l)return;lines.push(l);if(lines.length>MAX)lines.shift();
 if(filterEl.value&&!l.includes(filterEl.value))return;
 if(log.childElementCount>MAX)log.removeChild(log.firstChild);
 const d=document.createElement("div");d.className=levelOf(l);d.textContent=l;
 log.appendChild(d);if(stick)log.scrollTop=log.scrollHeight;}
log.onscroll=()=>{stick=log.scrollHeight-log.scrollTop-log.clientHeight<40};
document.getElementById("clear").onclick=()=>{log.replaceChildren();lines=[]};
document.getElementById("save").onclick=()=>{
 const b=new Blob([lines.join("\n")+"\n"],{type:"text/plain"}),a=document.createElement("a");
 a.href=URL.createObjectURL(b);a.download="jarvis-panel-log.txt";a.click();URL.revokeObjectURL(a.href);};
document.getElementById("copy").onclick=async()=>{await navigator.clipboard.writeText(lines.join("\n"));
 status.textContent="copied "+lines.length+" lines";};
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _headers(self, code=200, ctype="text/html; charset=utf-8", length=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.end_headers()

    def _que(self, line):
        """Buffer + broadcast to live subscribers."""
        if not line.strip():
            return
        with _lock:
            _buffer.append(line[:4096])
            subs = list(_subs)
        for q in subs:
            q.put(line[:4096])

    # ---- GET ----
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            with _lock:
                body = json.dumps({"lines": len(_buffer), "clients": len(_subs),
                                   "since": round(_started)})
        elif path == "/":
            body = PAGE
        elif path == "/logs.txt":
            with _lock:
                body = "\n".join(_buffer) + "\n"
        elif path == "/logs":
            self._sse()
            return
        else:
            body = "not found"
        b = body.encode()
        self._headers(200, "application/json" if path == "/health"
                      else ("text/plain; charset=utf-8" if path == "/logs.txt" else "text/html; charset=utf-8"),
                      len(b))
        self.wfile.write(b)

    def _sse(self):
        q = queue.Queue()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with _lock:
                pending = list(_buffer)
                _subs.append(q)
            for line in pending:
                self._wr("data: " + line + "\n\n")
            last_hb = time.time()
            while True:
                try:
                    self._wr("data: " + q.get(timeout=0.25) + "\n\n")
                except queue.Empty:
                    pass
                if time.time() - last_hb > 15:
                    self._wr(": hb\n\n")
                    last_hb = time.time()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with _lock:
                try:
                    _subs.remove(q)
                except ValueError:
                    pass

    def _wr(self, chunk: str):
        self.wfile.write(chunk.encode())
        self.wfile.flush()

    def do_OPTIONS(self):
        self._headers(204, length=None)
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ---- POST: the browser (console.html) pushes lines here ----
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace")
        lines = body.splitlines()
        for line in lines:
            self._que(line.rstrip())
        self._headers(204, length=None)

    def log_message(self, fmt, *args):
        pass


def main():
    port = PORT_NUM
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    ThreadingHTTPServer.allow_reuse_address = True
    print(f"[logsrv] collecting on http://0.0.0.0:{port}/", flush=True)
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
