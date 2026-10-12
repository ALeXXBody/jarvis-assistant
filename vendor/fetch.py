import json, os, hashlib, urllib.request, concurrent.futures, time, threading

CHUNK = 2 << 20          # 2 MB range chunks
WORKERS = 12             # tuned: 8-16 conns tested fastest on this CDN
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

m = json.load(open('manifest.json'))
shards = sorted(m['shards'], key=lambda s: s['seq'])
os.makedirs('shards', exist_ok=True)

lock = threading.Lock()
done = [0]
t0 = time.time()

def fetch_range(fn, url, off, length, tries=10):
    end = off + length - 1
    last = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={
                'X-Gdisk-Fetch': '1', 'User-Agent': UA,
                'Range': f'bytes={off}-{end}'})
            with urllib.request.urlopen(req, timeout=180) as r:
                data = r.read()
            if len(data) != length:
                raise IOError(f'short read {len(data)}/{length}')
            with lock:
                with open(fn, 'r+b') as f:
                    f.seek(off); f.write(data)
                done[0] += len(data)
            return
        except Exception as e:
            last = e
            time.sleep(min(30, 2 + attempt * 3))
    raise RuntimeError(f'{fn}@{off}: {last}')

def shard_file(s):
    return f"shards/{s['seq']:02d}.bin"

def is_done(s):
    fn = shard_file(s)
    if os.path.exists(fn) and os.path.getsize(fn) == s['size']:
        return hashlib.sha256(open(fn, 'rb').read()).hexdigest() == s['sha256']
    return False

for s in shards:
    if not is_done(s):
        with open(shard_file(s), 'wb') as f:
            f.truncate(s['size'])

print(f"shards={len(shards)} total={m['size']/1048576:.1f}MB workers={WORKERS}", flush=True)

jobs = []
for s in shards:
    if is_done(s):
        print(f"seq {s['seq']} cached", flush=True); continue
    for off in range(0, s['size'], CHUNK):
        jobs.append((shard_file(s), s['url'], off, min(CHUNK, s['size'] - off)))

with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as ex:
    futs = [ex.submit(fetch_range, *j) for j in jobs]
    for i, fu in enumerate(concurrent.futures.as_completed(futs)):
        fu.result()
        if (i + 1) % 10 == 0:
            print(f"  {done[0]/1048576:.0f}MB / {m['size']/1048576:.0f}MB  [{time.time()-t0:.0f}s]", flush=True)

ok = True
for s in shards:
    fn = shard_file(s)
    h = hashlib.sha256(open(fn, 'rb').read()).hexdigest()
    o = h == s['sha256']
    ok &= o
    print(f"seq {s['seq']}: {'OK' if o else 'HASH-MISMATCH'}", flush=True)

if ok:
    with open('panel.zip', 'wb') as f:
        for s in shards:
            with open(shard_file(s), 'rb') as g:
                while True:
                    b = g.read(1 << 20)
                    if not b: break
                    f.write(b)
    print('assembled panel.zip', os.path.getsize('panel.zip'), flush=True)
else:
    print('NOT assembling due to hash mismatch', flush=True)
