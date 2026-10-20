#!/usr/bin/env python3
"""后台全量重分析：起 worker 子进程 → idb_open(dll, run_auto_analysis) → 轮询 → idb_save 新库。
输出: /home/banshan/tmp/wetype-win/extracted/3.0.1.1/libwxhld_new.i64
"""
import json, os, socket, subprocess, sys, time, urllib.request

IDADIR = "/home/banshan/app/ida-pro-9.4"
PYBIN = os.path.expanduser("~/.local/share/uv/tools/ida-pro-mcp/bin/python")
DLL = "/home/banshan/tmp/wetype-win/extracted/3.0.1.1/libwxhld.dll"
OUT = "/home/banshan/tmp/wetype-win/extracted/3.0.1.1/libwxhld_new.i64"
LOG = "/tmp/idaw_reanalyze.txt"

def log(msg):
    with open(LOG, "a") as f: f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def rpc(url, payload, timeout=1800):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=timeout).read())

def call(url, name, args, timeout=1800):
    r = rpc(url, {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                  "params": {"name": name, "arguments": args}}, timeout)
    c = r.get("result", {}).get("content", [])
    return json.loads(c[0]["text"]) if c and c[0].get("type") == "text" else r

def main():
    port = free_port()
    url = f"http://127.0.0.1:{port}/mcp"
    proc = subprocess.Popen(
        [PYBIN, "-m", "ida_pro_mcp.idalib_server", "--host", "127.0.0.1", "--port", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env=dict(os.environ, IDADIR=IDADIR))
    try:
        for _ in range(120):
            try:
                rpc(url, {"jsonrpc": "2.0", "id": 0, "method": "tools/list"}, timeout=5)
                break
            except Exception:
                time.sleep(0.5)
        else:
            log("worker not ready"); sys.exit(1)
        log("worker up, opening dll with auto analysis...")
        r = call(url, "idb_open", {"input_path": DLL, "run_auto_analysis": True})
        log(f"idb_open: {json.dumps(r)[:300]}")
        for i in range(720):  # 最多 60 分钟
            time.sleep(5)
            try:
                h = call(url, "server_health", {})
                log(f"[{i*5}s] {json.dumps(h.get('health', h))[:200]}")
                if h.get("health", {}).get("auto_analysis_ready"):
                    break
            except Exception as e:
                log(f"health err {e}")
        log("saving...")
        r = call(url, "idb_save", {"path": OUT})
        log(f"idb_save: {json.dumps(r)[:300]}")
        log("DONE")
    finally:
        proc.terminate()

if __name__ == "__main__":
    main()
