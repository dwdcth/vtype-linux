#!/bin/sh
# install_win_engine.sh —— 把 Windows 引擎 + wine 宿主 + 云栈装进 ~/.local/lib/wetype-ime/win64
# 布局（宿主与 wetype_server.exe 必须同目录，spawn_server 按邻居找）：
#   win64/
#     wetype_win_host.exe   行协议↔session_v2 管道翻译（本仓库 win/host 编译产物）
#     wetype_server.exe 等  官方 Windows 引擎全套（不在 git，装时复制）
#     prefix/               wine 前缀（随发行树，管道 key 与安装路径无关，实测稳定）
#     wetype-cloud.py cloud/  云栈（配对/云候选/剪贴板，与引擎解耦）
# 用法: sh win/scripts/install_win_engine.sh [引擎源目录] [prefix源目录]
#   默认源: $HOME/tmp/wetype-win/extracted/3.0.1.1 和 $HOME/tmp/wetype-win/prefix
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC_ENGINE="${1:-$HOME/tmp/wetype-win/extracted/3.0.1.1}"
SRC_PREFIX="${2:-$HOME/tmp/wetype-win/prefix}"
DEST="${WETYPE_WIN_DEST:-$HOME/.local/lib/wetype-ime/win64}"

HOST_BIN="$ROOT/win/bin/wetype_win_host.exe"
[ -f "$HOST_BIN" ] || { echo "缺少 $HOST_BIN；先跑 win/scripts/build.sh" >&2; exit 1; }
[ -f "$SRC_ENGINE/wetype_server.exe" ] || { echo "引擎源缺 wetype_server.exe: $SRC_ENGINE" >&2; exit 1; }
[ -d "$SRC_PREFIX" ] || { echo "缺少 wine 前缀: $SRC_PREFIX" >&2; exit 1; }
command -v wine >/dev/null || { echo "未安装 wine" >&2; exit 1; }

mkdir -p "$DEST"
# 引擎全套（保留目录结构：BuildIn 词库等是相对路径）
cp -a "$SRC_ENGINE/." "$DEST/"

# 剔除历史实验产物（探针 exe / 调试转储）与符号文件；
# 官方文件（wetype_server.exe、Uninstall.exe、CrashRpt 等）全部保留。
rm -f "$DEST"/*.pdb 2>/dev/null || true
for x in framing msgmatrix pipebridge pipecrack pipelist pipelist2 pipelist3 \
         pipetest probe probe2 probe-zig session_probe select_test enum_pipes \
         wmcopy winlist; do
  rm -f "$DEST/$x.exe" 2>/dev/null || true
done
rm -f "$DEST"/e.txt "$DEST"/e?.txt "$DEST"/e??.txt "$DEST"/server_run.txt 2>/dev/null || true

# wine 前缀（如已存在则跳过，保用户登录等状态）
if [ ! -d "$DEST/prefix" ]; then
  cp -a "$SRC_PREFIX" "$DEST/prefix"
else
  echo "prefix 已存在，保留不动: $DEST/prefix"
fi

# 宿主（覆盖旧版）：
#   wetype_win_host.exe —— Route W（管道翻译，fcitx5 插件 Windows 路线）
#   wxhost.exe          —— Route C（进程内直调引擎，rime 插件后端，
#                          win/rime 的 WeTypeClient 默认在本目录找它）
install -m 755 "$HOST_BIN" "$DEST/wetype_win_host.exe"
install -m 755 "$ROOT/win/bin/wxhost.exe" "$DEST/wxhost.exe"

# 云栈：入口 + 模块（与 arm64 安装同构；wine 路线不需要 cloud_proxy.py/QEMU 代理）
install -m 755 "$ROOT/scripts/wetype-cloud.py" "$DEST/wetype-cloud.py"
mkdir -p "$DEST/cloud"
cp "$ROOT"/cloud/*.py "$DEST/cloud/"
rm -f "$DEST/cloud/cloud_net.c" 2>/dev/null || true

echo "已安装到 $DEST"
echo "fcitx5 插件启用方式（二选一）："
echo "  1) 显式: export WETYPE_WIN_ENGINE_DIR=$DEST 后重启 fcitx5"
echo "  2) 兜底: 未装 ARM64 引擎时插件自动选用 win64 目录"
echo "rime 插件（rime-wetype，用微信引擎模型词库）："
echo "  构建: sh win/rime/build_and_test.sh（含 E2E）"
echo "  会话: 默认也在 $DEST 找 wxhost.exe，无需另设环境变量"
echo "云功能（与引擎解耦，照常可用）："
echo "  python3 $DEST/wetype-cloud.py pair --name 'Linux 输入法' --code 关联码"
echo "  python3 $DEST/wetype-cloud.py clipboard enable"
