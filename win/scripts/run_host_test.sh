#!/bin/sh
# run_host_test.sh —— wine 宿主端到端测试：起引擎 → 起宿主 → 喂行协议命令 → 校验应答
# 用法: sh win/scripts/run_host_test.sh
# 依赖: ~/tmp/wetype-win/{extracted/3.0.1.1,prefix}（只读资产）、win/bin/wetype_win_host.exe
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN=/tmp/wetype-run

# 防上一次的僵尸引擎占住管道
pkill -x wetype_server.e 2>/dev/null; pkill -x wineserver 2>/dev/null; sleep 1

rm -rf "$RUN"; mkdir -p "$RUN"
cp -r "$HOME/tmp/wetype-win/extracted/3.0.1.1" "$RUN/engine"
cp -r "$HOME/tmp/wetype-win/prefix" "$RUN/prefix"
cp "$ROOT/bin/wetype_win_host.exe" "$RUN/engine/"

cd "$RUN/engine" || exit 1
WINEPREFIX=$RUN/prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
  nohup wine wetype_server.exe logtostderr > /tmp/wsvr.txt 2>&1 &

# 等引擎起来（最多 30s）
i=0; while [ $i -lt 30 ] && ! pgrep -x wetype_server.e >/dev/null; do sleep 1; i=$((i+1)); done
sleep 3
echo "engine_up=$(pgrep -x wetype_server.e >/dev/null && echo yes || echo no)"

# 宿主喂命令：READY 期望在 60s 内（含管道等待）
{
  sleep 45   # 给宿主留足连接时间
  echo "PING"
  sleep 1
  echo "B nihao"
  sleep 3
  echo "S 0"
  sleep 2
  echo "B ni"
  sleep 3
  echo "C"
  sleep 1
  echo "B hao"
  sleep 3
  echo "S 0"
  sleep 2
  echo "PING"
  sleep 1
  echo "Q"
} | WINEPREFIX=$RUN/prefix WINEDEBUG=-all timeout -s KILL 120 \
    wine "$RUN/engine/wetype_win_host.exe" > /tmp/whost_out.txt 2>/tmp/whost_err.txt
RC=$?

echo "=== 宿主 stdout ==="; cat /tmp/whost_out.txt
echo "=== 宿主 stderr(尾20行) ==="; tail -20 /tmp/whost_err.txt
echo "=== 校验 ==="
out=$(cat /tmp/whost_out.txt)
check() { echo "$out" | grep -q "$1" && echo "PASS: $2" || echo "FAIL: $2 (找 '$1')"; }
check '^READY' "宿主就绪"
check '^PONG' "PING/PONG ×2"
echo "$out" | grep -q '^CAND' && echo "PASS: 收到候选" || echo "FAIL: 无候选"
echo "$out" | grep -P '\t你好' >/dev/null && echo "PASS: 候选含'你好'" || echo "FAIL: 候选无'你好'"
n_ok=$(echo "$out" | grep -c '^OK')
[ "$n_ok" -ge 3 ] && echo "PASS: S/C 应答 OK ×$n_ok" || echo "FAIL: OK 只有 ×$n_ok"
pkill -x wetype_server.e 2>/dev/null
echo "host_rc=$RC"
