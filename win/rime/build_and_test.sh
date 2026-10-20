#!/bin/sh
# rime-wetype 插件构建与 E2E 测试（可复现脚本，2026-10-21 实测路径）
#
# 前置系统依赖（Arch/同名包）：
#   leveldb marisa opencc yaml-cpp cmake git curl
# 产物：librime（含 wetype 插件静态编入 librime.so）
# 环境：
#   WETYPE_LIBRIME_DIR  librime 检出目录（缺省 /tmp/librime）
#   WETYPE_BOOST_DIR    boost regex 安装前缀（缺省 /tmp/boostinst）
#   WETYPE_RUN_ENGINE   wxhost+引擎目录（E2E 用，缺省 /tmp/wetype-run/engine）
set -e

RIME_DIR="${WETYPE_LIBRIME_DIR:-/tmp/librime}"
BOOST_DIR="${WETYPE_BOOST_DIR:-/tmp/boostinst}"
ENGINE_DIR="${WETYPE_RUN_ENGINE:-/tmp/wetype-run/engine}"
HERE="$(cd "$(dirname "$0")" && pwd)"   # win/rime

# 1. librime 检出（一次性）
if [ ! -d "$RIME_DIR" ]; then
    git clone --depth 1 -b 1.17.0 https://github.com/rime/librime.git "$RIME_DIR"
    mkdir -p "$RIME_DIR/plugins/wetype"
fi

# 2. boost regex（librime 正则依赖；系统包缺 regex 静态库时自编）
if [ ! -d "$BOOST_DIR/lib" ]; then
    echo "== 自编 boost.regex（首次约几分钟）"
    tmp=$(mktemp -d)
    curl -sL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz \
        | tar xz -C "$tmp" --strip-components=1
    cd "$tmp" && ./bootstrap.sh --with-libraries=regex >/dev/null
    ./b2 --with-regex install --prefix="$BOOST_DIR" >/dev/null
fi

# 3. 插件同步 + 构建
cp "$HERE"/wetype/src/*.cc "$HERE"/wetype/src/*.h "$RIME_DIR/plugins/wetype/src/"
cp "$HERE"/wetype/CMakeLists.txt "$RIME_DIR/plugins/wetype/"
cd "$RIME_DIR"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LOGGING=OFF -DBUILD_TEST=OFF \
      -DCMAKE_PREFIX_PATH="$BOOST_DIR" >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
echo "== 构建 OK：$RIME_DIR/build/lib/librime.so（插件静态编入）"

# 4. E2E（可选：传 --test）
if [ "$1" = "--test" ]; then
    USER_DIR=/tmp/rime-user
    mkdir -p "$USER_DIR"
    cp "$HERE"/wetype.schema.yaml "$USER_DIR/"
    cat > "$USER_DIR/default.custom.yaml" <<'EOF'
patch:
  schema_list:
    - schema: wetype
EOF
    find "$USER_DIR/build" -depth -delete 2>/dev/null || true
    # 首次需手动部署：rime_deployer --build <user> /usr/share/rime-data
    LD_LIBRARY_PATH="$RIME_DIR/build/lib" \
    WETYPE_WIN_ENGINE_DIR="$ENGINE_DIR" WINEDEBUG=-all \
        sh -c 'printf "nihao\n1\n" | "$0"' "$RIME_DIR/build/bin/rime_api_console"
fi
