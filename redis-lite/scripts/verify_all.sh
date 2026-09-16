#!/usr/bin/env bash
# 在 Linux 上做一次完整验证：cmake 构建 + 单测 + ZRANK 复杂度 + 关闭路径。
#
# 存在理由：CI 之外，本地/虚拟机上需要一个「一条命令跑完全部验证」的入口。
# 也避免了在远端 shell 里手拼长命令行（引号在多层 shell 间传递极易出错，
# 本项目在 Windows -> ssh -> bash 这条链上反复踩过）。
#
# 用法: bash scripts/verify_all.sh [--quick]
#   --quick  跳过 100 万规模的 ZRANK 对比（慢机器上省时间）
set -u
cd "$(dirname "$0")/.." || exit 1

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

FAILS=0
step() { echo; echo "########## $* ##########"; }

# ---------------------------------------------------------------- 1. cmake 构建
step "1/5 cmake Release 构建"
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release > /tmp/vb_cm.log 2>&1 || {
    echo "cmake 配置失败:"; tail -20 /tmp/vb_cm.log; exit 1; }
cmake --build build -j"$(nproc)" > /tmp/vb_cb.log 2>&1 || {
    echo "构建失败:"; grep -iE "error" /tmp/vb_cb.log | head -20; exit 1; }
WARN=$(grep -ciE "warning" /tmp/vb_cb.log || true)
echo "构建成功; 告警数=$WARN"
[ "$WARN" != "0" ] && grep -iE "warning" /tmp/vb_cb.log | head -10
echo "产物:"; ls build/bin/

# ---------------------------------------------------------------- 2. 单元测试
step "2/5 单元测试"
./build/bin/rl-tests > /tmp/vb_test.log 2>&1
RC=$?
tail -4 /tmp/vb_test.log
[ "$RC" -ne 0 ] && { echo "单测失败"; FAILS=$((FAILS+1)); }

# ---------------------------------------------------------------- 3. -Werror
step "3/5 -Werror 构建（零告警基线）"
rm -rf build-werror
cmake -B build-werror -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-Werror -Wall -Wextra" > /dev/null 2>&1
cmake --build build-werror -j"$(nproc)" > /tmp/vb_werr.log 2>&1
if [ $? -eq 0 ]; then
    echo "-Werror 构建通过"
else
    echo "-Werror 构建失败:"; grep -iE "error" /tmp/vb_werr.log | head -15; FAILS=$((FAILS+1))
fi

# ---------------------------------------------------------------- 4. ZRANK 复杂度
step "4/5 ZRANK 复杂度验证"
if [ "$QUICK" = "1" ]; then
    ./build/bin/rl-bench-zrank 1000 10000 100000
else
    ./build/bin/rl-bench-zrank 1000 10000 100000 1000000
fi
[ $? -ne 0 ] && FAILS=$((FAILS+1))

# ---------------------------------------------------------------- 5. 关闭路径 + 端到端
step "5/5 关闭路径 + 端到端（ASan + UBSan）"
if command -v nc > /dev/null 2>&1; then
    rm -rf build-debug
    bash scripts/build.sh debug > /tmp/vb_asan.log 2>&1
    if [ $? -eq 0 ]; then
        ./build-debug/bin/rl-tests > /tmp/vb_atest.log 2>&1 && tail -3 /tmp/vb_atest.log
        bash scripts/test_shutdown.sh ./build-debug/bin | tail -8
        [ $? -ne 0 ] && FAILS=$((FAILS+1))

        echo "--- 端到端 ZSET（真协议 + AOF 恢复）---"
        bash scripts/e2e_zset.sh ./build-debug/bin | tail -6
        [ $? -ne 0 ] && FAILS=$((FAILS+1))
    else
        echo "ASan 构建失败:"; grep -iE "error" /tmp/vb_asan.log | head -10; FAILS=$((FAILS+1))
    fi
else
    echo "跳过（缺 nc）"
fi

echo
echo "========================================"
if [ "$FAILS" -eq 0 ]; then
    echo "verify_all: ALL PASS"
else
    echo "verify_all: $FAILS step(s) FAILED"
fi
echo "========================================"
exit "$FAILS"
