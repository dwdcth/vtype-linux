#!/bin/bash
# run_spawn_test.sh —— 复刻插件 spawn 分支的机制验证（不依赖 fcitx5 构建）：
#   1. 不预启动服务器 → 宿主应自己 CreateProcess wetype_server.exe（cwd=引擎目录）
#   2. execlp("wine", host) + stdin/stdout 管道 + WINEPREFIX=prefix
#   3. READY → PING/PONG → B nihao → CAND(你好) → S 0 → OK → C → OK → Q
# 用法: bash win/scripts/run_spawn_test.sh [安装目录]（默认 $WETYPE_WIN_ENGINE_DIR，再默认 win64 目录）
set -u
DIR="${1:-${WETYPE_WIN_ENGINE_DIR:-$HOME/.local/lib/wetype-ime/win64}}"
[ -f "$DIR/wetype_win_host.exe" ] || { echo "缺 $DIR/wetype_win_host.exe" >&2; exit 1; }
[ -d "$DIR/prefix" ] || { echo "缺 $DIR/prefix" >&2; exit 1; }
command -v wine >/dev/null || { echo "缺 wine" >&2; exit 1; }

pkill -x wetype_server.e 2>/dev/null; pkill -x wineserver 2>/dev/null; sleep 1

IN=$(mktemp -u /tmp/whost_in.XXXXXX)
OUT=/tmp/whost_spawn_out.txt
mkfifo "$IN"
: > "$OUT"

echo "== spawn: wine wetype_win_host.exe（复刻插件 execlp 分支）"
env WINEPREFIX="$DIR/prefix" WINEDEBUG=-all \
  timeout -s KILL 150 wine "$DIR/wetype_win_host.exe" < "$IN" > "$OUT" 2>/tmp/spawn_err.txt &
HOSTPID=$!
exec 3>"$IN"    # 保持 FIFO 写端

# 命令按节奏喂入；应答落 OUT，最后统一断言
{
  sleep 75          # READY 等待（含宿主自起引擎 ~15-20s）
  echo "PING";      sleep 2
  echo "B nihao";   sleep 4
  echo "S 0";       sleep 2
  echo "C";         sleep 2
  echo "B hao";     sleep 4
  echo "S 0";       sleep 2
  echo "Q"
} >&3
exec 3>&-
wait $HOSTPID 2>/dev/null
RC=$?

echo "== 宿主应答 =="; cat "$OUT"
echo "== 校验 =="
ok=1
chk() { grep -q "$1" "$OUT" && echo "PASS: $2" || { echo "FAIL: $2"; ok=0; }; }
chk '^READY'    "READY（含自起引擎）"
chk '^PONG'     "PING→PONG"
chk '^CAND'     "B→CAND"
chk '^CAND.*你好' "候选含 你好"
[ "$(grep -c '^OK' "$OUT")" -ge 3 ] && echo "PASS: S/C→OK ×$(grep -c '^OK' "$OUT")" || { echo "FAIL: OK 数不足"; ok=0; }
rm -f "$IN"
pkill -x wetype_server.e 2>/dev/null
[ $ok -eq 1 ] && echo "ALL PASS" || { echo "HAS FAILURES (host_rc=$RC)"; exit 1; }
