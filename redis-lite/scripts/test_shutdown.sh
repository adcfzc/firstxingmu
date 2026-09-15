#!/usr/bin/env bash
# Shutdown path verification under ASan/UBSan. ASCII only (encoding-safe).
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=./build-debug/bin
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1
FAILS=0

check() {
    port="$1"; log="$2"; label="$3"
    n=$(grep -icE 'FATAL|AddressSanitizer|runtime error|LeakSanitizer' "$log" || true)
    if [ "$n" != "0" ]; then
        echo "[FAIL] $label: $n finding(s)"
        grep -iE 'FATAL|AddressSanitizer|runtime error|LeakSanitizer' "$log" | head -4
        FAILS=$((FAILS+1))
    else
        echo "[ OK ] $label"
    fi
}

echo "### T1: clean shutdown, no connections ###"
$BIN/redis-lite --port 7511 --threads 2 --log-level info > /tmp/sd1.log 2>&1 &
P=$!; sleep 2
kill -TERM $P; sleep 2
if kill -0 $P 2>/dev/null; then echo "[FAIL] T1 process still running"; kill -9 $P; FAILS=$((FAILS+1)); fi
check 7511 /tmp/sd1.log "T1 clean shutdown"

echo "### T2: shutdown with live connections + traffic ###"
$BIN/redis-lite --port 7512 --threads 2 --log-level info > /tmp/sd2.log 2>&1 &
P=$!; sleep 2
for i in 1 2 3 4; do ( sleep 20 | timeout 20 nc 127.0.0.1 7512 > /dev/null 2>&1 & ) 2>/dev/null; done
$BIN/rl-bench -h 127.0.0.1 -p 7512 -c 4 -n 5000 -P 32 -d 64 -t set > /dev/null 2>&1
sleep 1
kill -TERM $P; sleep 2.5
if kill -0 $P 2>/dev/null; then echo "[FAIL] T2 process still running (deadlock?)"; kill -9 $P; FAILS=$((FAILS+1)); fi
check 7512 /tmp/sd2.log "T2 shutdown with connections"

echo "### T3: shutdown with AOF, verify flush ###"
rm -f /tmp/sd3.aof
$BIN/redis-lite --port 7513 --threads 2 --aof /tmp/sd3.aof --aof-fsync always --log-level info > /tmp/sd3.log 2>&1 &
P=$!; sleep 2
$BIN/rl-bench -h 127.0.0.1 -p 7513 -c 2 -n 2000 -P 16 -d 64 -t set > /dev/null 2>&1
kill -TERM $P; sleep 2
if kill -0 $P 2>/dev/null; then echo "[FAIL] T3 process still running"; kill -9 $P; FAILS=$((FAILS+1)); fi
check 7513 /tmp/sd3.log "T3 shutdown with AOF"
echo "    AOF size: $(stat -c%s /tmp/sd3.aof 2>/dev/null || echo 0) bytes"

echo
if [ "$FAILS" -eq 0 ]; then echo "RESULT: ALL PASS"; else echo "RESULT: $FAILS FAILED"; fi
exit $FAILS
