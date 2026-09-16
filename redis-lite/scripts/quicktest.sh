#!/usr/bin/env bash
# 不依赖 cmake 的快速编译 + 单测脚本。
#
# 存在理由：
#   - 目标环境（老发行版、容器、CI 最小镜像）可能没有 cmake；
#   - 调 bug 时需要「改一行、编一次、跑一个套件」的最小回路。
#
# 用法:
#   bash scripts/quicktest.sh              # 编译全部并跑全部单测
#   bash scripts/quicktest.sh skiplist     # 只跑名字含 skiplist 的用例
set -u
cd "$(dirname "$0")/.." || exit 1

FILTER="${1:-}"
OUT_DIR="${TMPDIR:-/tmp}/rl-quicktest"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/rl-tests"
LOG="$OUT_DIR/build.log"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"

# 收集源文件（显式列出，避免 glob 漏掉 .cc/.cpp 混用的问题 —— 本项目踩过）
SRCS=(
  src/aof.cpp src/command.cpp src/config.cpp src/connection.cpp
  src/connection_manager.cpp src/event_loop.cpp src/listener.cpp src/log.cpp
  src/platform.cpp src/poller.cpp src/resp.cpp src/server.cpp src/store.cpp
  src/zset.cpp src/skip_list.cpp
)
TESTS=(
  tests/test_main.cc tests/test_resp.cc tests/test_store.cc
  tests/test_aof.cc tests/test_zset.cc tests/test_skiplist.cc
)

echo "compiling with $CXX ..."
"$CXX" $CXXFLAGS -I include -I tests "${SRCS[@]}" "${TESTS[@]}" \
    -o "$BIN" -lpthread > "$LOG" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "BUILD FAILED (rc=$rc); first errors:"
    grep -E "error|Error" "$LOG" | head -20
    exit 1
fi
WARNS=$(grep -ciE "warning" "$LOG" || true)
echo "build OK, warnings=$WARNS"
[ "$WARNS" != "0" ] && grep -iE "warning" "$LOG" | head -10

# /tmp may be mounted noexec on some systems; be explicit.
chmod +x "$BIN" 2>/dev/null || true

if [ -n "$FILTER" ]; then
    echo "running tests matching: $FILTER"
    "$BIN" "$FILTER"
else
    echo "running all tests"
    "$BIN"
fi
