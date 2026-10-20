#!/bin/sh
# 构建 wine 路线全部 PE 产物：宿主（host/）+ 探针（probes/）。依赖任一：
#   zig     （本机：mise 的 zig 0.17；缓存目录需可写，脚本已设到 win/.zig-cache）
#   mingw-w64-gcc（pacman -S mingw-w64-gcc）
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

# 宿主是正式产物，-O2 + 告警全开
echo "== bin/wetype_win_host.exe"
$CC -O2 -Wall -o bin/wetype_win_host.exe host/wetype_win_host.c

# wxhost：路线 C 模块化宿主（直调 libwxhld.dll，正式产物）
echo "== bin/wxhost.exe"
$CC -O1 -Wall -o bin/wxhost.exe wxhost/main.c wxhost/wxime_engine.c \
    wxhost/wxime_event.c wxhost/wxhost_serve.c -lpsapi

# 探针是实验工具
for f in probes/*.c; do
  out="bin/$(basename "$f" .c).exe"
  echo "== $out"
  $CC -O1 -o "$out" "$f"
done
echo "宿主: bin/wetype_win_host.exe（install_win_engine.sh 装到 ~/.local/lib/wetype-ime/win64）"
echo "探针: win/bin/*.exe，用 scripts/run_probe_test.sh 跑，见 README"
