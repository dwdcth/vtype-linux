#!/bin/sh
# run_probe_test.sh —— 沙箱内一键测试：/tmp 复制引擎→起 wetype_server→跑探针
# 用法: sh win/scripts/run_probe_test.sh [探针exe名] [额外参数...]
# 说明: 本会话沙箱只允许写工作区与每次调用独立的 /tmp，故引擎与前缀
#       每次调用复制到 /tmp 再跑（~5s）。真实系统上无需此步骤。
set -e
PROBE="${1:-session_probe.exe}"; shift 2>/dev/null || true
SRC=~/tmp/wetype-win
RUN=/tmp/wetype-run

rm -rf "$RUN"; mkdir -p "$RUN"
cp -r "$SRC/extracted/3.0.1.1" "$RUN/engine"
cp -r "$SRC/prefix" "$RUN/prefix"
cp "/home/banshan/项目/vtype-linux/win/bin/$PROBE" "$RUN/engine/"

cd "$RUN/engine"
# dbg* 探针自带调试器会自己 spawn 服务器；预起会占住单实例互斥体导致被调试进程秒退
case "$PROBE" in
  dbg*)
    echo "(dbg probe: skip pre-start server)"
    ;;
  *)
    WINEPREFIX=$RUN/prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
      nohup wine wetype_server.exe logtostderr > /tmp/wsvr.txt 2>&1 &
    # 等服务器起来（管道就绪），最多 20s
    i=0
    while [ $i -lt 40 ]; do
      if pgrep -x wetype_server.e >/dev/null; then break; fi
      sleep 0.5; i=$((i+1))
    done
    sleep 2
    ;;
esac

RC=0
WINEPREFIX=$RUN/prefix WINEDEBUG=-all \
  timeout -s KILL 90 wine "$RUN/engine/$PROBE" "$@" 2>/dev/null || RC=$?

echo "server_alive=$(pgrep -x wetype_server.e >/dev/null && echo yes || echo no)"

echo "=== server tail ==="
tail -5 /tmp/wsvr.txt || true
pkill -x wetype_server.e 2>/dev/null || true
exit $RC
