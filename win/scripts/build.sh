#!/bin/sh
# 构建 wine 路线 PE 宿主（host/wetype_win_host.c）。依赖任一：
#   zig     （本机：mise 的 zig 0.17；缓存目录需可写，脚本已设到 win/.zig-cache）
#   mingw-w64-gcc（pacman -S mingw-w64-gcc）
# rime 插件（win/rime/）的构建见 win/rime/build_and_test.sh。
set -e
cd "$(dirname "$0")/.."
mkdir -p bin
export ZIG_GLOBAL_CACHE_DIR="$PWD/.zig-cache"
export ZIG_LOCAL_CACHE_DIR="$PWD/.zig-cache"
if command -v zig >/dev/null; then
  CC="zig cc -target x86_64-windows-gnu"
elif command -v x86_64-w64-mingw32-gcc >/dev/null; then
  CC="x86_64-w64-mingw32-gcc -static"
else
  echo "缺交叉编译器：zig 或 mingw-w64-gcc" >&2; exit 1
fi

echo "== bin/wetype_win_host.exe"
$CC -O2 -Wall -o bin/wetype_win_host.exe host/wetype_win_host.c

echo "宿主: bin/wetype_win_host.exe（install_win_engine.sh 装到 ~/.local/lib/wetype-ime/win64）"
