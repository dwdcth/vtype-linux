#!/usr/bin/env python3
"""自包含 IDA 批查：单进程内起 worker(子进程) -> 开库 -> 跑一批工具调用 -> 输出 JSON。
用法: python3 idaw_batch.py <calls.json>   # calls.json: [{"tool":"disasm","args":{...}},...]
输出: /tmp/idaw_batch_out.json + 摘要打印
"""
import json, os, socket, subprocess, sys, time, urllib.request

IDADIR = "/home/banshan/app/ida-pro-9.4"
PYBIN = os.path.expanduser("~/.local/share/uv/tools/ida-pro-mcp/bin/python")
IDB = "/home/banshan/tmp/wetype-win/extracted/3.0.1.1/libwxhld.dll.i64"

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def rpc(url, payload, timeout=600):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=timeout).read())

def call_tool(url, name, args):
    # JSON-RPC tools/call
    r = rpc(url, {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                  "params": {"name": name, "arguments": args}})
    if "error" in r:
        return {"error": str(r["error"])}
    content = r.get("result", {}).get("content", [])
    for c in content:
        if c.get("type") == "text":
            try:
                return json.loads(c["text"])
            except Exception:
                return {"raw": c["text"]}
    return r.get("result")

def main():
    calls = json.load(open(sys.argv[1]))
    port = free_port()
    url = f"http://127.0.0.1:{port}/mcp"
    env = dict(os.environ, IDADIR=IDADIR)

    def spawn():
        return subprocess.Popen(
            [PYBIN, "-m", "ida_pro_mcp.idalib_server",
             "--host", "127.0.0.1", "--port", str(port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)

    proc = spawn()

    def wait_ready():
        for _ in range(240):
            try:
                rpc(url, {"jsonrpc": "2.0", "id": 0, "method": "tools/list"}, timeout=5)
                return True
            except Exception:
                time.sleep(0.5)
        return False

    def run(name, args):
        nonlocal proc
        for attempt in range(3):
            try:
                return call_tool(url, name, args)
            except Exception:
                proc = spawn()
                if not wait_ready():
                    continue
                try:
                    call_tool(url, "idb_open", {"input_path": IDB})
                except Exception:
                    pass
        return {"error": "unreachable"}

    try:
        if not wait_ready():
            print("worker not ready"); sys.exit(1)
        r = run("idb_open", {"input_path": IDB})
        print("idb_open:", "ok" if not isinstance(r, dict) or "error" not in r or r.get("success") else r, flush=True)
        out = []
        for c in calls:
            r = run(c["tool"], c.get("args", {}))
            out.append({"req": c, "res": r})
        json.dump(out, open("/tmp/idaw_batch_out.json", "w"), ensure_ascii=False, indent=1)
        print(f"done: {len(out)} calls -> /tmp/idaw_batch_out.json")
    finally:
        proc.terminate()

if __name__ == "__main__":
    main()
