#!/usr/bin/env bash
# 在 Linux 虚拟机上采集压测数据。
#
# 为什么不做「客户端与服务端同机」的大并发：
#   VM 只有 2 核。压测工具自己也要吃 CPU，客户端线程开太多会与
#   服务端抢核，测出来的是调度开销而不是服务端能力。
#   所以客户端并发数保持在 4~8，用 pipeline 深度来施压。
set -u

BIN=~/redis-lite/build/bin
PORT=7100
OUT=/tmp/bench_results.txt
: > "$OUT"

start_server() {
    local threads="$1"
    "$BIN/redis-lite" --port "$PORT" --threads "$threads" --log-level warn \
        > /tmp/server_${threads}.log 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "server failed to start" >&2
    return 1
}

stop_server() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}

# 注意：不同 --threads 必须重启服务端，线程数是启动参数
for THREADS in 1 2 4; do
    stop_server
    start_server "$THREADS" || continue

    echo "########## threads=$THREADS ##########" | tee -a "$OUT"
    # 确认服务端确实按预期线程数起来了
    grep -o "loops=[0-9]*" /tmp/server_${THREADS}.log | head -1 | tee -a "$OUT"

    for PIPE in 1 16 64; do
        echo "----- pipeline=$PIPE -----" | tee -a "$OUT"
        # -c 6：客户端并发；-n 100000：总请求数
        "$BIN/rl-bench" -h 127.0.0.1 -p "$PORT" -c 6 -n 100000 -P "$PIPE" -d 64 -t set \
            2>&1 | grep -E "throughput|avg |p50|p90|p99 |p99.9|max |completed" | tee -a "$OUT"
        echo "" >> "$OUT"
    done
    stop_server
    sleep 0.5
done

echo "=== DONE ===" | tee -a "$OUT"
