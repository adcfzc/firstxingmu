#!/usr/bin/env bash
# ZSET 端到端验证：真服务端 + 真 RESP 协议 + AOF 崩溃恢复。
#
# 为什么单测够了还要做这个：
#   单测验证的是 ZSet 类本身。命令层、协议层、持久化层是另一条链路 ——
#   ZSet 内部换实现（std::set -> 字典+跳表）后，这条链路必须重新走一遍。
#
# 依赖 nc（netcat）；缺失时跳过并返回 0。
#
# 用法: bash scripts/e2e_zset.sh [binary-dir]   （默认 ./build/bin）
set -u
cd "$(dirname "$0")/.." || exit 1

BIN_DIR="${1:-./build/bin}"
BIN="$BIN_DIR/redis-lite"
PORT=7600
AOF=/tmp/e2e_zset.aof
[ -x "$BIN" ] || { echo "先构建: bash scripts/build.sh"; exit 1; }
command -v nc > /dev/null 2>&1 || { echo "跳过（缺 nc）"; exit 0; }

FAILS=0

# 构造 RESP 数组命令
resp() {
    local out="*$#\r\n"
    local a
    for a in "$@"; do out="$out\$${#a}\r\n$a\r\n"; done
    printf "$out"
}

# 发送一条命令，返回服务端原始回复（去掉 CRLF）
ask() {
    resp "$@" | timeout 3 nc -q1 127.0.0.1 "$PORT" 2>/dev/null | tr -d '\r'
}

expect() {
    local want="$1"; shift
    local got
    got=$(ask "$@")
    if [ "$got" = "$want" ]; then
        echo "  [ OK ] $*  ->  $got"
    else
        echo "  [FAIL] $*"
        echo "         got  $got"
        echo "         want $want"
        FAILS=$((FAILS + 1))
    fi
}

start_server() {
    # ★ 先清理同端口的遗留进程。
    #
    #   本项目实测踩过这个坑：上一轮 e2e 失败后服务端进程没被回收，
    #   占着 7600 端口。新脚本启动的实例绑定失败并立刻退出，
    #   而脚本只检查「端口可达」——于是全程在跟**旧二进制**对话，
    #   得出「WITHSCORES 不支持」这种与代码不符的结论，白查一轮。
    #
    #   教训与「二进制不存在却报 ALL PASS」同源：
    #   **测试必须确保被测对象就是自己刚启动的那个。**
    pkill -f "redis-lite --port $PORT" 2>/dev/null
    sleep 0.3

    "$BIN" --port "$PORT" --aof "$AOF" --aof-fsync always --log-level warn \
        > /tmp/e2e_srv.log 2>&1 &
    SRV=$!

    for _ in $(seq 1 50); do
        if ! kill -0 "$SRV" 2>/dev/null; then
            echo "[FATAL] 服务端启动后立即退出（端口被占用？）"; cat /tmp/e2e_srv.log; exit 1
        fi
        if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "[FATAL] 服务端未在超时内监听 $PORT"; cat /tmp/e2e_srv.log; exit 1
}

stop_server() {
    kill -TERM "${SRV:-0}" 2>/dev/null
    wait "${SRV:-0}" 2>/dev/null
}

rm -f "$AOF"

echo "### 第一轮：写数据 ###"
start_server

# ZADD 返回新增个数（重复 member 只更新分数，不计入）
expect ":3"  ZADD board 100 alice 85 bob 95 carol
expect ":0"  ZADD board 90 alice
# ZSCORE / ZCARD
expect "\$2
90" ZSCORE board alice
expect ":3"  ZCARD board
# ZRANK 是 0-based：bob(85) < alice(90) < carol(95)
expect ":0"  ZRANK board bob
expect ":1"  ZRANK board alice
expect ":2"  ZRANK board carol
expect ":2"  ZREVRANK board bob
# 不存在的 member
expect "\$-1" ZSCORE board nobody
expect "\$-1" ZRANK board nobody
# ZRANGE 默认**只返回 member**（与 Redis 一致）；WITHSCORES 才带分数且头 ×2
expect "*3
\$3
bob
\$5
alice
\$5
carol" ZRANGE board 0 -1
expect "*6
\$3
bob
\$2
85
\$5
alice
\$2
90
\$5
carol
\$2
95" ZRANGE board 0 -1 WITHSCORES
# 倒序
expect "*3
\$5
carol
\$5
alice
\$3
bob" ZREVRANGE board 0 -1
# ZRANGEBYSCORE：默认 member；排除端点用 (90
expect "*2
\$5
alice
\$5
carol" ZRANGEBYSCORE board 90 +inf
expect "*1
\$5
carol" ZRANGEBYSCORE board '(90' +inf
expect ":2" ZCOUNT board 90 95
# ZINCRBY 改变顺序：bob 85+100=185，应排到最后
expect "\$3
185" ZINCRBY board 100 bob
expect ":2" ZRANK board bob
# 类型错误
expect "+OK" SET plain v
expect "-WRONGTYPE Operation against a key holding the wrong kind of value" ZADD plain 1 x
# 非法 score
expect "-ERR value is not a valid float" ZADD board abc x
expect "-ERR value is not a valid float" ZADD board inf x
# ZREM
expect ":1" ZREM board carol
expect ":0" ZREM board carol
# 集合清空后 key 应消失
expect ":1" DEL board
expect ":0" EXISTS board

echo "### 第二轮：kill -9 后重启，验证 AOF 恢复 ###"
expect ":2" ZADD lb 10 x 20 y
expect "+OK" SET keep hello
stop_server
# 用 SIGKILL 模拟崩溃
kill -9 "$SRV" 2>/dev/null
sleep 0.5

start_server
expect ":2" ZCARD lb
expect ":0" ZRANK lb x
expect ":1" ZRANK lb y
expect "\$2
20" ZSCORE lb y
expect "\$5
hello" GET keep
stop_server

echo
echo "========================================"
if [ "$FAILS" -eq 0 ]; then
    echo "e2e zset: ALL PASS"
else
    echo "e2e zset: $FAILS check(s) FAILED"
fi
echo "========================================"
exit "$FAILS"
