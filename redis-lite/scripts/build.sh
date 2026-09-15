#!/usr/bin/env bash
# redis-lite — Linux 构建脚本（生产/压测形态，走 epoll ET）
#
# 用法:
#   ./scripts/build.sh            # Release
#   ./scripts/build.sh debug      # Debug（带 ASan，便于抓内存问题）
#   ./scripts/build.sh test       # 编译并跑单元测试

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-release}"
BUILD_DIR="$ROOT/build"

if [[ "$MODE" == "debug" ]]; then
    CMAKE_BUILD_TYPE=Debug
    EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    BUILD_DIR="$ROOT/build-debug"
else
    CMAKE_BUILD_TYPE=Release
    EXTRA_FLAGS="-O2 -DNDEBUG"
fi

echo "==> configuring ($CMAKE_BUILD_TYPE) in $BUILD_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_CXX_FLAGS="$EXTRA_FLAGS"

echo "==> building"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "==> artifacts"
ls -lh "$BUILD_DIR/bin"

if [[ "$MODE" == "test" ]]; then
    echo
    echo "==> running unit tests"
    "$BUILD_DIR/bin/rl-tests"
fi

echo
echo "启动服务端:"
echo "  $BUILD_DIR/bin/redis-lite --port 6379 --threads 4 --aof"
echo
echo "压测:"
echo "  $BUILD_DIR/bin/rl-bench -c 50 -n 200000 -P 16 -d 64 -t set"
