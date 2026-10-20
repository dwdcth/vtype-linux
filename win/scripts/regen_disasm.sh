#!/bin/sh
# 再生成全量反汇编与导出表（输入：~/tmp/wetype-win/extracted 的安装包内容）
# 产物不进 git（86MB+52MB），放 ~/tmp/wetype-win/
set -e
B=${1:-$HOME/tmp/wetype-win/extracted}
OUT=${2:-$HOME/tmp/wetype-win}
cd "$OUT"
echo "== wetype_server.exe 全量反汇编"
objdump -d -M intel "$B/3.0.1.1/wetype_server.exe" > server_disasm.txt
echo "== wetype_tip_core.dll 全量反汇编（客户端参考）"
objdump -d -M intel "$B/WetypeCore_3.0.1.1/x64/wetype_tip_core.dll" > tip_disasm.txt
echo "done: $(du -sh server_disasm.txt tip_disasm.txt | cut -f1 | tr '\n' ' ')"
