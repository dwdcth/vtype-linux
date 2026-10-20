#!/bin/sh
# run_wxhost_test.sh —— Route C 宿主端到端测试：wxhost --serve 行协议全命令校验
# 与 run_host_test.sh（Route W）同构：喂命令 → 校验应答。Route C 引擎在
# wxhost 进程内直调，无需预起 wetype_server。
# 用法: sh win/scripts/run_wxhost_test.sh
# 依赖: ~/tmp/wetype-win/{extracted/3.0.1.1,prefix}（只读资产）、win/bin/wxhost.exe
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN=/tmp/wetype-run

pkill -x wxhost.exe 2>/dev/null; pkill -x wetype_server.e 2>/dev/null; sleep 1

rm -rf "$RUN/engine" "$RUN/prefix"; mkdir -p "$RUN"
cp -r "$HOME/tmp/wetype-win/extracted/3.0.1.1" "$RUN/engine"
cp -r "$HOME/tmp/wetype-win/prefix" "$RUN/prefix"
find "$RUN/engine/Tencent" -depth -delete 2>/dev/null   # 干净用户词典
cp "$ROOT/bin/wxhost.exe" "$RUN/engine/"

cd "$RUN/engine" || exit 1
{
  sleep 45   # 进程内初始化：词典 + 5s 用户词典等待 + 会话
  echo "PING"
  sleep 1
  echo "B nihao"
  sleep 3
  echo "S 0"
  sleep 2
  echo "B nihaoma"
  sleep 3
  echo "S 0"
  sleep 2
  echo "C"
  sleep 1
  echo "B n"
  sleep 3
  echo "PING"
  sleep 1
  echo "Q"
} | WINEPREFIX=$RUN/prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    timeout -s KILL 120 wine "$RUN/engine/wxhost.exe" --serve . \
    > /tmp/wxhost_out.txt 2>/tmp/wxhost_err.txt
RC=$?

echo "=== wxhost stdout ==="; cat /tmp/wxhost_out.txt
echo "=== 断言 ==="
pass=0; fail=0
chk() {  # chk <描述> <应含内容>
  if grep -q "$2" /tmp/wxhost_out.txt; then echo "PASS: $1"; pass=$((pass+1));
  else echo "FAIL: $1（缺 \"$2\"）"; fail=$((fail+1)); fi
}
chk "READY 就绪"            "^READY"
chk "PING→PONG"             "^PONG"
chk "B nihao 出候选"        "CAND.*你好"
chk "S 0 整段完成 OK"       "^OK"
chk "B nihaoma 出候选"      "CAND.*你"
chk "S 0 部分选词回 CAND(剩 ma)" "CAND.*妈\|CAND.*吗\|CAND.*马"
chk "C 重置 OK"             "^OK"
chk "重置后 B n 仍出候选"   "CAND.*你"
echo "=== 结果: pass=$pass fail=$fail rc=$RC ==="
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "HAS FAILURES"
