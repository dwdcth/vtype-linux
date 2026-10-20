#!/usr/bin/env python3
"""idaw.py —— 直连 idalib worker 的 HTTP MCP 小助手
用法: python3 idaw.py <tool名> [JSON参数]
     python3 idaw.py --list          # 列全部工具
     python3 idaw.py --call 工具 参数
默认 worker: http://127.0.0.1:58319/mcp（可用 IDAW_URL 覆盖）
"""
import json, sys, os, urllib.request

URL = os.environ.get("IDAW_URL", "http://127.0.0.1:58319/mcp")

def rpc(method, params=None, _id=1):
    body = {"jsonrpc": "2.0", "id": _id, "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(
        URL, data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Accept": "application/json, text/event-stream"})
    with urllib.request.urlopen(req, timeout=1700) as r:
        raw = r.read().decode()
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            line = line[5:].strip()
        if line.startswith("{"):
            return json.loads(line)
    return {"raw": raw}

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    if sys.argv[1] == "--list":
        r = rpc("tools/list")
        for t in r["result"]["tools"]:
            print(f"{t['name']:40s} {json.dumps(t.get('inputSchema',{}).get('properties',{}), ensure_ascii=False)[:150]}")
        return 0
    name = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    r = rpc("tools/call", {"name": name, "arguments": args})
    if "result" not in r:
        print(json.dumps(r, ensure_ascii=False)[:2000]); return 1
    for c in r["result"].get("content", []):
        if c.get("type") == "text":
            print(c["text"])
    return 0

if __name__ == "__main__":
    sys.exit(main())
