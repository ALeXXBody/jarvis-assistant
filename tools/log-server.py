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
#!/usr/bin/env python3
"""
JARVIS panel log collector + browser serial console (self-hosted HTTPS).

Two listeners, one shared ring buffer:

  HTTP  :8766   tooling access     curl /health | /logs.txt | /logs (SSE)
  HTTPS :8767   the *page*         https://<host>:8767  <- the user opens this

Why HTTPS: browsers only expose Web Serial (navigator.serial) to secure
contexts, so the panel can only be opened from an https:// or file:// page.
Hosting the serial app here (self-signed certificate, user accepts the
warning once) removes every other setup step: the page opens the panel over
USB, renders the log, and POSTs new lines to itself (/lines) into the shared
buffer — which then serves /logs.txt and /health for tooling.

Stdlib only.  Cert/key are expected next to this file in .private/logsrv/
(NOT committed) and can be rebuilt with openssl.
"""
import json
import os
import queue
import ssl
import sys
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Lock
from urllib.parse import urlparse

PORT_NUM = 8766
PORT_TLS = 8767
MAX_LINES = 4000

_buffer = deque(maxlen=MAX_LINES)
_lock = Lock()
_subs = []
_started = time.time()


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>J.A.R.V.I.S. panel — console</title>
<style>
 :root{color-scheme:dark}
 *{box-sizing:border-box}
 body{margin:0;height:100vh;display:flex;flex-direction:column;font:14px/1.5 ui-monospace,Menlo,Consolas,monospace;background:#0b0f1a;color:#c9d4f2}
 header{display:flex;align-items:center;gap:.6rem;padding:.55rem .9rem;background:#101a33;border-bottom:1px solid #23335e;flex-wrap:wrap}
 header b{color:#7fd4ff;margin-right:.4rem;font-family:system-ui,sans-serif}
 button,input[type=text]{background:#141f3d;color:#dbe4ff;border:1px solid #2b3c6e;border-radius:6px;padding:.35rem .7rem;font:inherit}
 button:hover{background:#1b2a52;cursor:pointer}
 #connect{background:#0b7285;border-color:#0b7285;color:#eaf7ff;font-weight:600}
 #connect.off{background:#5c4b1e;border-color:#5c4b1e;color:#ffe9a8}
 #filter{min-width:12rem;flex:1}
 #status{color:#7a89b8}
 main{flex:1;overflow-y:auto;padding:.5rem .9rem;white-space:pre-wrap;word-break:break-all}
 .er{color:#ff7a90}.wr{color:#ffd479}.in{color:#c9d4f2}.db{color:#6f7fa8}.tx{color:#b7a6ff}
 footer{display:flex;gap:.4rem;padding:.55rem .9rem;background:#101a33;border-top:1px solid #23335e;flex-wrap:wrap;align-items:center}
 #send{flex:1;min-width:12rem}
 .hint{color:#7a89b8;font-family:system-ui,sans-serif;font-size:.85rem;flex-basis:100%}
</style></head>
<body>
<header><b>J.A.R.V.I.S. panel console</b>
<button id="connect">Connect</button>
<button id="clear">Clear</button><button id="save">Save log</button><button id="copy">Copy</button>
<input id="filter" type="text" placeholder="filter lines (substring)">
<label><input type="checkbox" id="pause"> pause</label>
<label><input type="checkbox" id="stream" checked> stream</label>
<span id="status">not connected</span></header>
<main id="log"></main>
<footer>
<input id="send" type="text" placeholder="type text and press Enter to send to the panel…">
<button class="k" data-c="l" title="start listening">listen</button>
<button class="k" data-c="s" title="stop / run turn">stop</button>
<button class="k" data-c="x" title="abort turn">abort</button>
<button class="k" data-c="a" title="approve tool">allow</button>
<button class="k" data-c="d" title="deny tool">deny</button>
<button class="k" data-c="r" title="restart ws client">reconnect</button>
<button class="k" data-c="u" title="check for update">update</button>
<button class="k" data-c="+" title="volume up">vol+</button>
<button class="k" data-c="-" title="volume down">vol−</button>
<span class="hint">keys mirror firmware console (jarvis_wake.c) · Web Serial over the USB cable only, no Wi-Fi ·
Chrome / Edge / Opera · lines stream back to this same server, where I read them via /logs.txt</span>
</footer>
<script>
"use strict";
const el={log:document.getElementById("log"),connect:document.getElementById("connect"),
 clear:document.getElementById("clear"),save:document.getElementById("save"),
 copy:document.getElementById("copy"),filter:document.getElementById("filter"),
 pause:document.getElementById("pause"),status:document.getElementById("status"),
 send:document.getElementById("send"),stream:document.getElementById("stream")};
let port=null,writer=null,running=false,partial="",stick=true;const MAXLINES=20000;
let lines=[];const BATCH=500,unsent=[];let posting=false,fails=0;
const PROTO=location.protocol==="https:"?"https":"http";
const EP=location.host;                       /* collector = this same server */

/* --- port --- */
el.connect.addEventListener("click",async()=>{
 if(running){await disconnect();return;}
 try{
  port=await navigator.serial.requestPort();
  await port.open({baudRate:115200});
  writer=port.writable.getWriter();
  await writer.write(new TextEncoder().encode("\r"));
  readLoop();setStatus("connected — streaming to collector");
  queueLine("=== browser attached to panel over Web Serial ===");
 }catch(e){setStatus(e.name==="NotFoundError"?"no device selected":"error: "+(e.message||e));}
});
async function readLoop(){running=true;el.connect.textContent="Disconnect";
 const dec=new TextDecoder("utf-8");
 try{while(running){const r=port.readable.getReader();
  try{while(running){const{value,done}=await r.read();
   if(value)feed(dec.decode(value,{stream:true}));if(done)throw new Error("closed");}}
  catch(e){try{r.releaseLock();}catch(_){}
   if(e.message==="closed")throw e;}
  await new Promise(r=>setTimeout(r,200));}}
 catch(e){setStatus("port closed — click Connect to reattach");}
}
async function disconnect(){running=false;
 try{writer?.releaseLock();}catch(_){}
 try{const r=port?.readable?.getReader();r?.cancel();r?.releaseLock?.();}catch(_){}
 try{await port?.close();}catch(_){}
 port=writer=null;el.connect.textContent="Connect";setStatus("not connected");}
function setStatus(m){el.status.textContent=m;if(port)el.connect.textContent="Disconnect";}

/* --- render --- */
const ANSI=/\x1b\[[0-9;]*m/g;
function feed(chunk){if(el.pause.checked){partial=chunk;return;}
 partial+=chunk;let i;
 while((i=partial.indexOf("\n"))>=0){append(partial.slice(0,i));partial=partial.slice(i+1);}
 if(partial.length>4096){append(partial);partial="";}}
function levelOf(l){if(/\bE ?\(\d+\)/.test(l))return"er";if(/\bW ?\(\d+\)/.test(l))return"wr";
 if(/\bD ?\(\d+\)/.test(l))return"db";if(/^>>>/.test(l))return"tx";return"in"}
function makeDiv(l){const d=document.createElement("div");d.className=levelOf(l);d.textContent=l;return d;}
function append(raw){const l=raw.replace(ANSI,"").replace(/\r$/,"");if(!l)return;
 lines.push(l);if(lines.length>MAXLINES)lines.splice(0,lines.length-MAXLINES);
 if(el.stream.checked){unsent.push(l);if(unsent.length>4000)unsent.splice(0,unsent.length-4000);}
 const f=el.filter.value;if(f&&!l.includes(f))return;
 if(el.log.childElementCount>MAXLINES)el.log.removeChild(el.log.firstChild);
 el.log.appendChild(makeDiv(l));if(stick)el.log.scrollTop=el.log.scrollHeight;}
el.log.addEventListener("scroll",()=>{stick=el.log.scrollHeight-el.log.scrollTop-el.log.clientHeight<40;});

/* --- stream batch --- */
setInterval(async()=>{if(!el.stream.checked||!unsent.length||posting)return;posting=true;
 const batch=unsent.splice(0,BATCH);
 try{const r=await fetch(`${PROTO}://${EP}/lines`,{method:"POST",body:batch.join("\n"),
   headers:{"Content-Type":"text/plain"}});
  if(!r.ok)throw new Error(r.status);fails=0;
  if(port)setStatus("connected — streaming to collector");
 }catch(e){batch.forEach(l=>unsent.push(l));
  if(++fails===1)setStatus(port?"collector unreachable — holding lines":"collector unreachable");}
 finally{posting=false;}},1000);

/* --- toolbar --- */
el.clear.onclick=()=>{el.log.replaceChildren();lines=[]};
el.pause.onchange=()=>setStatus(el.pause.checked?"paused (device still logging)":(port?"connected":"not connected"));
el.save.onclick=()=>{const b=new Blob([lines.join("\n")+"\n"],{type:"text/plain"}),a=document.createElement("a");
 a.href=URL.createObjectURL(b);a.download="jarvis-panel-log.txt";a.click();URL.revokeObjectURL(a.href);};
el.copy.onclick=async()=>{await navigator.clipboard.writeText(lines.join("\n"));
 setStatus("copied "+lines.length+" lines");};

/* --- send --- */
async function send(s){if(!writer){setStatus("not connected");return;}
 await writer.write(new TextEncoder().encode(s));}
el.send.addEventListener("keydown",e=>{if(e.key==="Enter"){const v=el.send.value;el.send.value="";if(v)send(v);}});
document.querySelectorAll("button.k").forEach(b=>b.onclick=()=>{el.send.value="";send(b.dataset.c);});
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
    http_port = PORT_NUM
    tls_port = PORT_TLS
    for i, argv_i in enumerate(sys.argv):
        if argv_i == "--port":
            http_port = int(sys.argv[i + 1])
        if argv_i == "--tls-port":
            tls_port = int(sys.argv[i + 1])

    servers = [ThreadingHTTPServer(("0.0.0.0", http_port), Handler)]

    cert = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", ".private", "logsrv", "cert.pem")
    key = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", ".private", "logsrv", "key.pem")
    if os.path.exists(cert) and os.path.exists(key):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        s = ThreadingHTTPServer(("0.0.0.0", tls_port), Handler)
        s.socket = ctx.wrap_socket(s.socket, server_side=True)
        servers.append(s)
        print(f"[logsrv] https://0.0.0.0:{tls_port}/  (serial app — open this)")
    else:
        print(f"[logsrv] no TLS cert in {os.path.dirname(cert)} — HTTPS off", flush=True)

    print(f"[logsrv] http://0.0.0.0:{http_port}/  (tooling: /logs.txt /health)", flush=True)
    for s in servers[:-1]:
        from threading import Thread
        Thread(target=s.serve_forever, daemon=True).start()
    servers[-1].serve_forever()


if __name__ == "__main__":
    main()
