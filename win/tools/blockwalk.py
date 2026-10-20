#!/usr/bin/env python3
"""按块地址列表批量 disasm 并紧凑打印（多帧 JSON 逐行解析）。"""
import json, subprocess, sys

URL = "http://127.0.0.1:53124/mcp"

def call(tool, args):
    p = subprocess.run(
        [sys.executable, "idaw.py", tool, json.dumps(args)],
        capture_output=True, text=True)
    frames = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            frames.append(json.loads(line))
        except Exception:
            pass
    if not frames:
        print("  [stderr]", p.stderr.strip()[:300])
        print("  [stdout]", p.stdout.strip()[:300])
    return frames

def disasm(addr, n=40):
    for f in call("disasm", {"addr": addr, "max_instructions": n}):
        asm = f.get("asm")
        if asm:
            for ln in asm.get("lines", []):
                print(f"  {ln['addr']}: {ln['instruction']}")
            return
    print(f"  (no asm frame for {addr})")

if __name__ == "__main__":
    for a in sys.argv[1:]:
        print(f"=== BLOCK {a} ===")
        disasm(a)
