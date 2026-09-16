#!/usr/bin/env bash
# Shutdown path verification under ASan/UBSan. ASCII only (encoding-safe).
#
# Usage: bash scripts/test_shutdown.sh [binary-dir]   (default: ./build-debug/bin)
#
# Why this test exists:
#   Under normal benchmark traffic, connections are closed by the CLIENT first,
#   which takes the "peer closed, seen inside the connection's own loop thread"
#   path. Server-initiated close is a DIFFERENT path, only reached on graceful
#   shutdown. This project had a real bug there: every benchmark passed, but the
#   process aborted the moment it received SIGTERM.
#
# Why the preflight checks below matter:
#   An earlier version of this script reported "ALL PASS" when the binaries did
#   not even exist -- the server never started, `kill` failed against a dead PID,
#   and the log-grep found no errors in an empty file. A test that passes when
#   nothing ran is WORSE than no test: it manufactures false confidence.
#   Every step below therefore asserts that the thing under test actually ran.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN="${1:-./build-debug/bin}"
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1
FAILS=0

# ---------- preflight: refuse to run unless the binaries exist ----------
if [ ! -x "$BIN/redis-lite" ]; then
    echo "[FATAL] server binary not found or not executable: $BIN/redis-lite"
    echo "        build it first (bash scripts/build.sh debug) or pass the right dir"
    exit 2
fi
if [ ! -x "$BIN/rl-bench" ]; then
    echo "[FATAL] bench binary not found or not executable: $BIN/rl-bench"
    exit 2
fi
echo "binary dir: $BIN"

# ---------- helpers ----------
# Wait until the server answers on a port. Returns non-zero on timeout.
wait_port() {
    port="$1"
    for _ in $(seq 1 60); do
        if (echo > "/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 0; fi
        sleep 0.2
    done
    return 1
}

check_log() {
    name="$1"
    log="$2"
    if [ ! -f "$log" ]; then
        echo "[FAIL] $name: log file missing ($log) -- server never started"
        FAILS=$((FAILS+1))
        return
    fi
    n=$(grep -icE 'FATAL|AddressSanitizer|runtime error|LeakSanitizer' "$log" || true)
    if [ "$n" != "0" ]; then
        echo "[FAIL] $name: $n finding(s)"
        grep -iE 'FATAL|AddressSanitizer|runtime error|LeakSanitizer' "$log" | head -4
        FAILS=$((FAILS+1))
    else
        echo "[ OK ] $name"
    fi
}

# Run one scenario: start server, optionally generate load, SIGTERM, verify exit.
run_scenario() {
    label="$1"; port="$2"; with_load="$3"; with_aof="$4"
    log="/tmp/sd_${port}.log"
    rm -f "$log"

    # 清理同端口遗留进程：否则新实例绑定失败，脚本却在与旧进程对话，
    # 得出错误结论（同 e2e_zset.sh 里记录的坑）。
    pkill -f "redis-lite --port $port" 2>/dev/null
    sleep 0.2

    if [ "$with_aof" = "yes" ]; then
        rm -f "/tmp/sd_${port}.aof"
        "$BIN/redis-lite" --port "$port" --threads 2 --aof "/tmp/sd_${port}.aof" \
            --aof-fsync always --log-level info > "$log" 2>&1 &
    else
        "$BIN/redis-lite" --port "$port" --threads 2 --log-level info > "$log" 2>&1 &
    fi
    P=$!

    # The server must actually come up. Without this check the whole test
    # silently degenerates into "kill a dead pid, grep an empty log, pass".
    if ! wait_port "$port"; then
        echo "[FAIL] $label: server did not start listening on $port"
        kill -9 $P 2>/dev/null
        grep -iE 'error|fatal' "$log" 2>/dev/null | head -3
        FAILS=$((FAILS+1))
        return
    fi
    if ! kill -0 "$P" 2>/dev/null; then
        echo "[FAIL] $label: our server process died (port taken by a leftover?)"
        FAILS=$((FAILS+1))
        return
    fi

    if [ "$with_load" = "yes" ]; then
        # Hold a few connections open, then run real traffic.
        for _ in 1 2 3 4; do
            ( sleep 20 | timeout 20 nc 127.0.0.1 "$port" > /dev/null 2>&1 & ) 2>/dev/null
        done
        "$BIN/rl-bench" -h 127.0.0.1 -p "$port" -c 4 -n 5000 -P 32 -d 64 -t set > /dev/null 2>&1
        sleep 1
    fi

    kill -TERM $P
    sleep 2.5
    if kill -0 $P 2>/dev/null; then
        echo "[FAIL] $label: process still alive after SIGTERM (deadlock?)"
        kill -9 $P 2>/dev/null
        FAILS=$((FAILS+1))
    fi
    check_log "$label" "$log"
    if [ "$with_aof" = "yes" ]; then
        # Assert the AOF is actually non-empty. Without this, a scenario that
        # writes nothing would still "pass" while proving nothing about the
        # flush-on-shutdown behaviour we are trying to verify.
        aof="/tmp/sd_${port}.aof"
        size=$(stat -c%s "$aof" 2>/dev/null || echo 0)
        if [ "$size" -gt 0 ]; then
            echo "    AOF size: $size bytes (flushed)"
        else
            echo "[FAIL] $label: AOF is empty -- nothing was persisted, test proves nothing"
            FAILS=$((FAILS+1))
        fi
    fi
}

echo "### T1: graceful shutdown, no connections ###"
run_scenario "T1 clean shutdown" 7511 no no

echo "### T2: graceful shutdown with live connections + traffic ###"
run_scenario "T2 shutdown with connections" 7512 yes no

echo "### T3: graceful shutdown with AOF (buffer must be flushed) ###"
# with_load=yes is required here: with no writes the AOF stays empty and the
# flush assertion below would be vacuous.
run_scenario "T3 shutdown with AOF" 7513 yes yes

echo
echo "========================================"
if [ "$FAILS" -eq 0 ]; then
    echo "shutdown path: ALL PASS (no sanitizer findings)"
else
    echo "shutdown path: $FAILS check(s) FAILED"
fi
echo "========================================"
exit "$FAILS"
